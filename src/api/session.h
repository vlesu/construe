// CoreApi: session of a single document (spec/core-api.md).
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "doc/document.h"
#include "doc/undo.h"
#include "fcstd/import.h"
#include "font/font.h"
#include "io/flat_dxf.h"
#include "gear/gear.h"
#include "io/mesh_export.h"
#include "io/sheet_export.h"
#include "io/stl.h"
#include "kernel/kernel.h"
#include "library/library.h"
#include "solutions/solutions.h"
#include "drawing/sheet.h"
#include "drawing/render.h"
#include "drawing/specification.h"
#include "drawing/dimensions.h"
#include "drawing/sheet_marks.h"
#include "pmi/pmi.h"
#include "printprep/printprep.h"
#include "regen/assembly_ref.h"
#include "regen/regen.h"
#include "sketch/sketch.h"
#include "sketch/trim.h"
#include "api/job_runner.h"
#include "api/tess_cache.h"
#include "api/projection_cache.h"
#include "api/regen_disk_cache.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace construe::rec {
struct Plan;
}

namespace construe::api {

using Json = doc::Json;

inline constexpr double INSERT_GAP_FRACTION = 0.2;  // assembly.md §8

inline constexpr int REGEN_THREADS = 1;
inline constexpr size_t TESS_CACHE_MAX_ENTRIES = 64;
// Material probes on one side of the profile (core-api.md §5 extrudeSideMaterial, §8).
inline constexpr int SIDE_PROBE_MAX = 64;
inline constexpr double SIDE_PROBE_CHORD = 0.5;  // mm
inline constexpr double SIDE_PROBE_EPS = 1e-4;   // mm
inline constexpr size_t THREAD_NEAREST = 2;      // nearest nominal sizes in the threadFit answer (FX2-FEAT-001)
inline constexpr double HOLE_AXIS_TOL = 1e-4;          // mm, coaxial holes (solutions.md §7.11а, S7F-ASM-008)
inline constexpr double HOLE_AXIS_ANGLE_TOL = 1e-6;    // |a × b| of unit axes, parallel

using PartResultPtr = std::shared_ptr<const regen::PartResult>;

// Reference to a state (§5.1).
struct StateRef {
    enum class Kind { Initial, After, Final, Preview, PreviewTool, Flat };  // Flat — flat pattern of the final state (§5.1)
    Kind kind = Kind::Final;
    std::string id;  // featureId / PreviewId

    static StateRef initial() { return {Kind::Initial, {}}; }
    static StateRef final() { return {Kind::Final, {}}; }
    static StateRef after(std::string f) { return {Kind::After, std::move(f)}; }
    static StateRef flat() { return {Kind::Flat, {}}; }
    static Result<StateRef> fromJson(const Json& j);
};

struct DocumentInfo {
    std::shared_ptr<const doc::Document> document;
    uint64_t revision = 0;
    bool modified = false;
};

struct UndoLabel {
    std::string key;
    std::map<std::string, std::string> params;
};

struct EditContext {
    std::optional<std::string> sketch;
};

struct SketchState {
    uint64_t revision = 0;
    Plane frame;
    sketch::SketchResult result;
    std::map<std::string, sketch::Projection> projections;  // geometry of projected entities (sketch.md §2.5)
    // Weak dimensions of the open sketch (core-api §5 sketchState, sketch.md §4.4, S4-WEAK-001): ID "w<N>"; dimension
    // values are in result.reference under their IDs; weakDof — degrees of freedom they close. Empty if not open.
    std::vector<sketch::Constraint> weak;
    int weakDof = 0;
};

// Dimension label sizes for the "Fully define" layout (core-api §4.6.2, FX4-SK-001), mm in sketch coordinates:
// textHeight — height of the label rectangle, charWidth — width of one character; ≤ 0 — from the sketch extent.
struct FullyDefineLayout {
    double textHeight = 0;
    double charWidth = 0;
    int maxRows = 0;  // > 0 — max rows (columns) of candidates; weak dimensions — WEAK_LAYOUT_ROWS (app-ux §6.6а)
};

struct BodyInfo {
    std::string bodyId;
    BBox3 bbox;
    double volume = 0, area = 0;
    int faces = 0, edges = 0, vertices = 0;
};

struct TopologyInfo {
    naming::EntityKind kind = naming::EntityKind::Face;
    std::string bodyId;
    std::optional<kernel::SurfaceInfo> surface;
    std::optional<kernel::CurveInfo> curve;
    std::optional<Point3> position;
};

// Measurement of one or two picked entities (core-api.md §5 measure, FX10-MEAS-001).
inline constexpr double MEASURE_PARALLEL_TOL = 1e-9;  // |u × v| of unit directions: parallel
inline constexpr double MEASURE_COPLANAR_TOL = 1e-6;  // mm: lines closer than this are coplanar
inline constexpr double MEASURE_BODY_CHORD = 0.01;    // mm: chord tolerance of the body distance sampling
struct MeasureItem {
    // length | radius | diameter | area | point | volume | mass | centroid | distance | angle | bodyDistance
    std::string kind;
    double value = 0;               // mm, mm², mm³, kg, rad; point and centroid — 0
    Point3 point = Point3::Zero();  // point, centroid
    // distance: the two points the distance is measured between — from the first selected to the second (FX12-MEAS-001,
    // core-api §5 measure); none — not known (bodyDistance).
    std::optional<std::array<Point3, 2>> span;
};

// Lost reference of a feature (core-api.md §5 lostReferences, regen.md §8а, S7F-FEAT-003).
struct LostReference {
    std::string owner = "feature";  // feature | mate | weld (regen.md §8а «Assembly objects»)
    std::string feature, ref;       // feature — the ID of the owner object
    std::vector<std::string> paths;     // JSON Pointers of the string values of the definition equal to ref
    std::string kind;                   // face | edge | vertex | unknown
    std::optional<io::BodyMesh> ghost;  // the entity in the feature's reference state: triangles, edge polylines, vertex
};
// One replacement of rebindReferences (core-api.md §4.4).
struct Rebind {
    std::string path, ref;
};

// Selection → value of a command input (core-api.md §5 selectionCandidates, selection.md §7.1, FX7-SEL-001).
inline constexpr size_t SEL_MAX_PICKS = 3;              // selection.md §10
inline constexpr double SEL_ON_TOL = 1e-4;              // mm, = KERNEL_MIN_DIMENSION: a point lies on a plane/line
struct SelectionPreview {
    std::optional<Plane> plane;                     // plane input
    std::optional<std::pair<Point3, Vec3>> axis;    // axis input: point and unit direction
    std::optional<std::pair<Point3, Vec3>> dir;     // direction input: arrow start and unit direction
    std::optional<Point3> point;                    // point input
};
struct SelectionCandidate {
    std::string rule;      // row of the selection.md §3 tables: "П2", "Н8", "О7", "Т1"
    int variant = 1;
    std::string labelKey;  // ui.sel.*
    Json labelParams = Json::object();  // {"a": <pick ref>, "b": …} — the client names them
    bool direct = false;   // one reference to a picked element, nothing is built
    bool confirm = false;  // waits for «Yes» (selection.md §4 item 3)
    Json value;            // PlaneRef | DirRef | AxisRef | PointRef; {"aux": i} — the i-th of aux
    std::vector<Json> aux; // owned reference features to build (plane/axis definitions without owner)
    SelectionPreview preview;
};
struct SelectionResult {
    std::string status = "empty";  // empty | prefix | candidates | invalid
    std::vector<std::string> need; // ui.sel.need.* — what to add (prefix) or what fits (invalid)
    bool extensible = false;       // the set is a prefix of a larger row (the field stays active, selection.md §5.1 item 6)
    std::vector<SelectionCandidate> candidates;
};
struct SelectionFilter {
    bool directOnly = false;  // only direct candidates (fields without an owning feature, selection.md К19)
};

// Thread fit for a cylindrical face (core-api.md §5 threadFit, regen.md §4.14 item 3, FX2-FEAT-001).
struct ThreadFitOption {
    std::string designation;
    bool nominal = false;   // internal face modeled at the thread nominal (not at the tap drill)
    double tapDrill = 0;    // tap drill diameter, mm
};
struct ThreadFitNearest {
    std::string designation;  // representative of the nominal size — first table row with this d (coarse pitch)
    double d = 0, pitch = 0;
    double tapMin = 0, tapMax = 0, nomMin = 0, nomMax = 0;  // allowed diameters, mm
};
struct ThreadFit {
    double diameter = 0;  // face diameter, mm
    bool internal = false;
    std::vector<ThreadFitOption> options;   // matching designations in table order
    int suggested = -1;                     // index into options; −1 — none match
    std::vector<ThreadFitNearest> nearest;  // nearest nominal sizes (THREAD_NEAREST), by increasing deviation
};

struct RegionMesh {
    std::string regionId;
    std::vector<Point2> vertices;
    std::vector<std::array<int, 3>> triangles;
    Point2 interiorPoint = Point2::Zero();
};

struct FeatureStatusInfo {
    bool evaluated = false;  // false — notEvaluated
    regen::FeatureStatus status = regen::FeatureStatus::Ok;
    std::vector<regen::Message> messages;
};

struct PreviewResult {
    uint64_t revision = 0;
    regen::FeatureStatus status = regen::FeatureStatus::Ok;
    std::vector<regen::Message> messages;
    bool hasTool = false;
};

// sketchTrim answer (spec/core-api.md §4.6.2).
struct TrimResult {
    std::vector<std::string> removedConstraints;
    std::vector<std::string> created;
};

struct ExpressionCheck {
    double value = 0;          // internal units
    double valueInDocUnits = 0;
    expr::Dim dim;
};

// History reconstruction report (core-api.md §4.7г, reconstruction.md §2.3; S4-REC-002).
struct ReconstructReport {
    std::string element;                      // the new part
    std::string level;                        // full | partial | importOnly
    std::vector<std::pair<std::string, std::string>> features;  // (ID, type) of created sketchless features, in order
    bool base = false;                        // there is an "Import" base body
    std::vector<std::pair<std::string, std::string>> notes;     // (kind, details) of what was not recognized
    bool rounded = false;                     // dimension values are rounded (S4-REC-013, reconstruction.md §5.4)
    // Per part (group of identical solids of the source body), in order (S4-REC-012, FX7-IO-001); a single solid gives one.
    std::vector<ReconstructReport> parts;
    // Solids: of the part's group of identical solids, or of the whole body at the top level (= assembly instances;
    // reconstruction.md §5.6, §5.7, FX7-IO-001).
    int instances = 1;
    std::string assembly;                     // assembly of the parts of a multi-body product (S4-REC-014); empty — no
    // Healing of the body shape during reconstruction (S4-REC-017, reconstruction.md §1); top level of the report only.
    struct Heal {
        bool healed = false;        // shape healed; false — tolerance exceeds the healing tolerance or healing failed
        double toleranceBefore = 0;  // tolerance of the shape entities before healing, mm
        double toleranceAfter = 0;   // after healing (not healed — equals toleranceBefore)
    };
    std::optional<Heal> heal;  // absent — healing not requested or shape tolerance not above KERNEL_MAX_ENTITY_TOL
    bool replaced = false;     // ReconstructOptions::replace: the result replaced the body (FX10-IO-001)
    // Replacing: mate references moved to the result's faces by geometry; mates (assembly, mate ID) with a reference left
    // on the body's names — they report ASM_MATE_REF_LOST (FX10-IO-008).
    int remapped = 0;
    std::vector<std::pair<std::string, std::string>> lostRefs;
};

// History reconstruction parameters (core-api.md §4.7г; S4-REC-013).
struct ReconstructOptions {
    double roundTolerance = 0;  // allowed rounding of dimension values: mm (angles — degrees); 0 — exact values
    double healTolerance = 0;   // mm: body shape healing tolerance before recognition (S4-REC-017); 0 — no healing
    // A full result replaces the body (its name, place and assembly references); otherwise the document is unchanged
    // (core-api §4.7г, FX10-IO-001).
    bool replace = false;
    // s of wall time on all the bodies of a product: the bodies not begun by then stay imported (reconstruction.md §5.6).
    double productSeconds = 300;
    // With replace: a result that is not full is added beside the body (names without replacing), the body and its
    // references are kept («Recognize design», FX10-IO-008).
    bool keepPartial = false;
};

// STEP import parameters and report (core-api.md §4.7б; S4-REC-013).
struct StepImportOptions {
    double healTolerance = 0;  // mm: largest shape entity tolerance that is healed; 0 — no healing
    bool dedupe = false;       // identical products of the file become one object (core-api §4.7б, FX7-IO-001)
    bool nameByFile = false;   // root object named by the file, others "<file> - <product>" (FX10-IO-002)
    bool singlePart = false;   // a root assembly wrapping a single part gives only the part (FX10-IO-001)
};
struct StepImportReport {
    struct Body {
        std::string element;
        double tolerance = 0;        // entity tolerance of the read shape, mm
        bool healed = false;         // shape healed (tolerance brought to at most KERNEL_MAX_ENTITY_TOL)
        double healedTolerance = 0;  // tolerance after healing
    };
    std::vector<Body> bodies;  // bodies with entity tolerance above KERNEL_MAX_ENTITY_TOL, in creation order
};

// Mesh import report (core-api §4.7д; S4-MESH-001): per body — whether surfaces were recognized.
struct MeshImportReport {
    struct Body {
        std::string element;
        bool fitted = false;  // body made of surfaces; false — polyhedral body from mesh triangles
        int faces = 0;        // faces of the body
        int triangles = 0;    // shell triangles in the mesh
        int meshFaces = 0;    // mesh faces of a mixed body (free-form parts kept as triangles, S4-MESH-002)
        std::string reason;   // recognition error code (fitted = false)
    };
    std::vector<Body> bodies;  // in element creation order
};

// Exchange report of a FreeCAD file (core-api §4.7е, spec/fcstd.md §9, §12е.3): objects by groups, in file or
// document order.
struct FcstdReport {
    std::vector<fcstd::ReportEntry> entries;
};

// Long operations in two phases (core-api §4.7з, FX7-UI-006): preparation — a const call that does not change the
// session (any thread, with cancellation), application — the commit on the owner thread. The prepared result holds
// the revision it was made for; applying it to another revision — API_STALE.
struct StepImportPrepared {
    struct Body {
        std::string brep;
        std::optional<StepImportReport::Body> heal;
    };
    kernel::StepModel model;
    std::vector<std::optional<Body>> bodies;  // per product: a part with a shape that could be written; otherwise nullopt
    std::string source;
    uint64_t revision = 0;
    bool dedupe = false;  // StepImportOptions::dedupe of the preparation (FX7-IO-001)
    bool nameByFile = false;  // StepImportOptions::nameByFile (FX10-IO-002)
    int single = -1;          // singlePart: the only part product to create (its shape already moved); -1 — the whole file
};
struct MeshImportPrepared {
    struct Body {
        std::string name, brep;
        MeshImportReport::Body report;
    };
    std::vector<Body> bodies;
    std::string source;
    uint64_t revision = 0;
};
struct ReconstructPrepared {
    doc::Document document;  // the document with the new parts (and assembly)
    ReconstructReport report;
    uint64_t revision = 0;
    bool unchanged = false;  // replace without a full result: nothing to commit (FX10-IO-001)
};
struct FcstdPrepared {
    fcstd::ImportResult import;
};

using PreviewId = std::string;
using ExportId = std::string;

// Event handlers (§6). onDocumentChanged — owner thread; the rest — worker thread.
struct Callbacks {
    std::function<void(uint64_t)> onDocumentChanged;
    std::function<void(uint64_t)> onRegenStarted;
    std::function<void(PartResultPtr)> onRegenerated;
    std::function<void(const PreviewId&, const PreviewResult&)> onPreview;
    std::function<void(const ExportId&, const Status&)> onExportFinished;
};

// A weld acting in an assembly (weld.md §7, FX9-ASM-008): the record (references prefixed for a nested assembly), its
// document and the nested assembly instance path (empty — the assembly itself).
struct ShownWeld {
    Json weld;
    const doc::Document* doc = nullptr;
    std::string path;
};

class Session {
public:
    explicit Session(const kernel::Kernel& k, Callbacks callbacks = {});
    // Session owning the kernel (factory createLocalSession, api/local_session.h).
    Session(std::unique_ptr<kernel::Kernel> owned, Callbacks callbacks = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // §4.1
    Status newDocument(const std::string& lengthUnit = "mm", const std::string& angleUnit = "deg");
    Status openDocument(const std::string& path);
    // Recovery of autosaved work (core-api.md §4.1): session path is originalPath, document is "modified".
    Status openRecovery(const std::string& copyPath, const std::optional<std::string>& originalPath);
    // Canonical form of the current document (core-api.md §5) — for client autosave.
    std::string documentText() {
        syncAssemblyPositions();  // solved instance positions (assembly.md §7.5)
        return doc::serialize(doc());
    }
    Status saveDocument(const std::optional<std::string>& path = std::nullopt);
    Result<ExportId> exportStl(const std::string& path, const io::StlOptions& options);
    // STL and 3MF mesh export (core-api §4.7а; S2-IO-001…003, S2-IO-005).
    struct MeshExportOptions {
        io::MeshFileFormat fileFormat = io::MeshFileFormat::Stl;
        io::StlFormat stlFormat = io::StlFormat::Binary;
        std::optional<double> chordalDeviation, angularDeviation;
        std::string element;             // empty — the active object
        std::vector<std::string> items;  // part bodies or top-level instance paths; empty — all
        bool perPart = false;            // STL only: file per part (path is a directory)
        // "For printing with tolerances" (S6-PRINT-001, -002): print bodies (printprep.md §5); nothing — as modeled.
        std::optional<printprep::Options> print;
    };
    Result<ExportId> exportMeshes(const std::string& path, const MeshExportOptions& options);
    Result<std::vector<io::MeshCheck>> meshReport(const MeshExportOptions& options);
    // Flat pattern export to DXF (core-api §4.7е; S5-DXF-001).
    struct FlatExportOptions {
        std::string element;             // empty — the active object
        std::vector<std::string> items;  // keys of sheet metal parts (FlatPartInfo::key); empty — all
        bool bendLines = true;           // bend line layers BEND-UP, BEND-DOWN
        bool perPart = false;            // file per part (path is a directory)
    };
    struct FlatPartInfo {
        std::string key, name;
        int loops = 0;    // cut contours
        double area = 0;  // flat pattern area by the contours, mm²
    };
    Result<ExportId> exportFlatDxf(const std::string& path, const FlatExportOptions& options);
    Result<std::vector<FlatPartInfo>> flatParts(const FlatExportOptions& options);
    // Default file name (core-api §4.7а); sheet — drawing sheet (§4.7в): with several sheets its name is appended.
    std::string exportFileName(const std::string& element, const std::string& sheet = {}) const;
    // Export of an active drawing sheet to PDF or SVG (core-api §4.7в; S3-IO-001, S3-IO-002).
    Result<ExportId> exportSheet(const std::string& sheetId, const std::string& path, io::SheetFileFormat format);
    // All sheets of the given drawings of the document, in order, with the view models computed (core-api §4.7в,
    // FX7-DRW-004, FX7-DRW-005): for the PDF of several drawings and for printing.
    Result<std::vector<drawing::SheetGraphics>> drawingSheets(const std::vector<std::string>& drawings);
    // Several drawings into one PDF — a page per sheet (core-api §4.7в, FX7-DRW-005); asynchronous, as exportSheet.
    Result<ExportId> exportDrawingsPdf(const std::vector<std::string>& drawings, const std::string& path);
    // DXF of drawings by sheets (FX11-DXF-001, io.md §2г): sheets of the drawings in the given order, a file per sheet —
    // path, then path with "-2", "-3", … (io::sheetFilePath).
    Result<ExportId> exportDrawingsDxf(const std::vector<std::string>& drawings, const std::string& path);
    // Sheet title block field values without the sheet's own titleBlock (core-api §5, drawing.md §5.6; S3-DRW-002).
    Result<std::map<std::string, std::string>> titleBlockDefaults(const std::string& sheetId) const;
    // Text outlines of a sheet primitive for client rendering (drawing.md §3.1).
    static Result<drawing::TextOutline> textOutline(const drawing::Primitive& text);
    // Frame of a sheet text with lineCount drawn lines (drawing.md §6.23, FX9-DRW-007); empty — the text has no frame.
    static std::optional<std::array<Point2, 4>> sheetTextFrame(const doc::SheetText& text, size_t lineCount);
    // STEP (core-api §4.7б; S2-IO-004, S2-DOC-001).
    Result<ExportId> exportStep(const std::string& path, const std::optional<std::string>& element = std::nullopt);
    // Shape healing (S4-REC-013): options.healTolerance; report — bodies with excessive entity tolerance (optional).
    Result<std::vector<std::string>> importStep(const std::string& path, const StepImportOptions& options = {},
                                                StepImportReport* report = nullptr);
    // STL or 3MF mesh import (core-api §4.7д; S4-MESH-001): bodies from recognized surfaces or polyhedral ones.
    Result<MeshImportReport> importMesh(const std::string& path);
    // Two phases of the long operations (core-api §4.7з, FX7-UI-006): prepare* — without changing the session, from any
    // thread while the owner thread does not change it; cancellation — API_CANCELLED; apply* — on the owner thread.
    Result<StepImportPrepared> prepareImportStep(const std::string& path, const StepImportOptions& options = {},
                                                 const Cancel* cancel = nullptr) const;
    Result<std::vector<std::string>> applyImportStep(StepImportPrepared prepared, StepImportReport* report = nullptr);
    // timeLimit: recognition limit of one shell, seconds (mesh-fit.md §4.10); 0 — MESHFIT_TIME_LIMIT.
    Result<MeshImportPrepared> prepareImportMesh(const std::string& path, const Cancel* cancel = nullptr, double timeLimit = 0) const;
    Result<MeshImportReport> applyImportMesh(MeshImportPrepared prepared);
    Result<ReconstructPrepared> prepareReconstruct(const std::string& bodyElement, const ReconstructOptions& options = {},
                                                   const Cancel* cancel = nullptr) const;
    Result<ReconstructReport> applyReconstruct(ReconstructPrepared prepared);
    Result<FcstdPrepared> prepareOpenFcstd(const std::string& path, const Cancel* cancel = nullptr) const;
    Result<FcstdReport> applyOpenFcstd(FcstdPrepared prepared);
    // FreeCAD exchange (core-api §4.7е, spec/fcstd.md; S4A-UI-001, S4A-REP-001): opening a .FCStd as a new unsaved
    // document and exporting the document (or one element) to .FCStd; both return the exchange report.
    Result<FcstdReport> openFcstd(const std::string& path);
    Result<FcstdReport> exportFcstd(const std::string& path, const std::optional<std::string>& element = std::nullopt);
    // History reconstruction of an imported body (core-api §4.7г, reconstruction.md; S4-REC-002): new part alongside.
    // Rounding of dimension values — options.roundTolerance (S4-REC-013); multi-body — also an assembly (S4-REC-014).
    Result<ReconstructReport> reconstructBody(const std::string& bodyElement, const ReconstructOptions& options = {});
    Status setUnits(const std::string& lengthUnit, const std::string& angleUnit);
    // Forced full regeneration with cache clearing (core-api §4.4, S1-REGEN-008).
    Status rebuild();
    std::optional<std::string> path() const { return path_; }
    // Evaluator disk cache (core-api §4.1ж; S4-CACHE-001): empty dir — disabled.
    void setRegenCacheDir(const std::string& dir, uint64_t maxBytes = REGEN_DISK_CACHE_MAX_BYTES) {
        cacheDir_ = dir;
        cacheMaxBytes_ = maxBytes;
    }

    // §4.2
    Status undo();
    Status redo();
    std::optional<UndoLabel> undoLabel() const;
    std::optional<UndoLabel> redoLabel() const;
    void beginGroup(const std::string& labelKey);
    Status endGroup();

    // §4.3
    Result<std::string> addVariable(const std::string& name, const std::string& expression,
                                    const std::optional<std::string>& comment = std::nullopt, const std::string& type = "float",
                                    const std::vector<std::string>& values = {});
    Status updateVariable(const std::string& id, const std::optional<std::string>& name,
                          const std::optional<std::string>& expression,
                          const std::optional<std::optional<std::string>>& comment = std::nullopt);
    // Renaming a variable together with all its references in formulas (core-api.md §4.3, FX9-VAR-001): variables,
    // variant table cells, feature expressions of every element, mate values — one undo step.
    Status renameVariableEverywhere(const std::string& id, const std::string& name);
    Status deleteVariable(const std::string& id);
    // Types and variant tables (core-api.md §4.3, S6-PARAM-001, S6-PARAM-003).
    Status setVariableType(const std::string& id, const std::string& type, const std::vector<std::string>& values = {});
    struct VariantTableDef {
        std::optional<std::string> name;
        std::optional<std::vector<std::string>> columns;  // variable IDs
        std::optional<std::vector<doc::VariantRow>> variants;
        std::optional<std::string> active;
    };
    Result<std::string> addVariantTable(const VariantTableDef& def);
    Status updateVariantTable(const std::string& id, const VariantTableDef& def);
    Status setActiveVariant(const std::string& id, const std::string& row);
    Status deleteVariantTable(const std::string& id);
    // Text of a variant table (§4.3а): delimiter ',' (CSV) or '\t' (clipboard).
    Result<std::string> variantTableText(const std::string& id, char delimiter = ',') const;
    struct ParsedVariantTable {
        std::vector<std::string> columns;  // variable IDs
        std::vector<doc::VariantRow> variants;
    };
    Result<ParsedVariantTable> parseVariantTableText(const std::string& text) const;
    // Instance variables (S6-PARAM-004, assembly.md §2а).
    Status setInstanceVariables(const std::string& id, const std::optional<std::vector<doc::Instance::VariantRow>>& variantRows,
                                const std::optional<std::vector<doc::Instance::Override>>& overrides);
    struct InstanceVariables {
        std::string qualifier;
        std::vector<doc::Variable> variables;      // of the inserted object's document
        std::vector<doc::VariantTable> tables;
        std::vector<doc::Instance::VariantRow> variantRows;  // of the instance
        std::vector<doc::Instance::Override> overrides;
        std::set<std::string> standard;  // variable IDs defined by the standard (library sizes table, FX10-LIB-006): no formula
    };
    Result<InstanceVariables> instanceVariables(const std::string& id) const;
    // Variable IDs of the inserted object that an instance formula may not override (library.md §6.3, FX10-LIB-006).
    std::set<std::string> standardVariables(const doc::Instance& in) const;

    // §4.4
    // aux — owned reference features built from a selection (core-api §4.4, FX7-SEL-001): inserted right before the
    // feature with owner = its ID; {"aux": i} objects in the definition become {"feature": <ID of the i-th>}.
    Result<std::string> addFeature(const Json& definition, const std::vector<Json>& aux = {});
    Status updateFeature(const std::string& id, const Json& definition, const std::vector<Json>& aux = {});
    Status deleteFeature(const std::string& id);
    // Replace string values of the definition at JSON Pointers (core-api §4.4, S7F-FEAT-003): one undo step.
    Status rebindReferences(const std::string& id, const std::vector<Rebind>& replacements);
    // Lost dimensions of a drawing sheet deleted in one undo step (core-api §4.1г, S7F-FEAT-003): the deleted IDs.
    Result<std::vector<std::string>> deleteLostDimensions(const std::string& sheetId);
    Status renameFeature(const std::string& id, const std::string& name);

    // Document elements (core-api.md §4.1а, S2-DOC-001).
    Result<std::string> addElement(const std::string& kind = "part");
    Status renameElement(const std::string& id, const std::string& name);
    Status deleteElement(const std::string& id);
    Status moveElement(const std::string& id, int index);
    // Element groups of the objects panel (core-api.md §4.1а, document.md §2.1а, FX10-TREE-001).
    Result<std::string> createElementGroup(const std::vector<std::string>& elementIds, const std::string& name = {});
    Status renameElementGroup(const std::string& id, const std::string& name);
    Status dissolveElementGroup(const std::string& id);
    // Element into group `group` (empty — out of groups) before node `before` of that level (empty — at its end).
    Status placeElement(const std::string& id, const std::string& group, const std::string& before);
    // Group with its members before top-level node `before` (an ungrouped element or a group; empty — at the end).
    Status moveElementGroup(const std::string& id, const std::string& before);
    Status setActiveElement(const std::string& id);
    // Part properties (core-api.md §4.1а, S2-PART-001/002/004).
    Status setElementProperties(const std::string& id, const Json& props);
    // Part kind conversion (core-api.md §4.1а, S5-SHEET-006): kind "sheet" — «Convert to sheet metal» (the form is checked:
    // a «Sheet» feature with recognize; an invalid form — the feature's error, the document unchanged), "part" — to regular.
    Status convertPart(const std::string& id, const std::string& kind);
    // Whether «Преобразовать в листовую» can succeed for the active ordinary part (FX12-SH-001, sheet.md §4.9): the sheet
    // recognition of the body of the last regeneration; the error is that of convertPart (SHEET_RECOGNIZE with the face
    // name). API_STATE_UNAVAILABLE — no result yet; API_INVALID_ARGUMENT — not an ordinary part.
    Status sheetConvertCheck() const;
    Status setFaceColor(const std::string& face, const std::optional<std::string>& color);
    // Assemblies (core-api.md §4.1в, assembly.md).
    Result<std::string> insertInstance(const Json& ref, const std::optional<std::array<double, 3>>& position = std::nullopt);
    // Copying objects between documents, «Make local copy», «Replace with…»
    // (core-api.md §4.1в, §4.1г; assembly.md §5б; FX7-ASM-008…010).
    Result<Json> copyElement(const std::string& id) const;
    Result<std::string> pasteElement(const Json& data, const std::string& mode);
    Result<std::string> localizeInstance(const std::string& id);
    Result<std::string> localizeView(const std::string& id);
    Status replaceInstance(const std::string& id, const Json& ref);
    Status replaceView(const std::string& id, const Json& ref);
    Status moveInstance(const std::string& id, const std::array<double, 3>& position, const std::array<double, 4>& rotation);
    Status setInstanceFlags(const std::string& id, const Json& flags);
    // Render color of an instance of the active assembly (FX12-ASM-003, core-api §4.1): "#RRGGBB" (any case, stored
    // upper-case) or none — by the part; one undo step undo.instanceColor.
    Status setInstanceColor(const std::string& id, const std::optional<std::string>& color);
    Status renameInstance(const std::string& id, const std::string& name);
    Status deleteInstance(const std::string& id);
    // Instance order (FX9-ASM-011, core-api §4.1в): `id` before `beforeId` (empty — to the end); the new order or
    // API_INSTANCE_ORDER with the mate that would change its instance in the tree.
    Result<std::vector<std::string>> reorderedInstances(const std::string& id, const std::string& beforeId) const;
    Status reorderInstance(const std::string& id, const std::string& beforeId);
    // The construction element name of a mate reference "<instance path>/<name>" (assembly.md §7.1, FX9-ASM-001): origin,
    // plane:*, axis:*, a feature ID or "<sketch>:<entity>"; nullopt — a stable name of the body or not an instance ref.
    static std::optional<std::string> partConstructionName(const std::string& ref);
    // Parsing of an assembly reference against the evaluated assembly and its top-level instance (assembly.md §3.1,
    // regen::parseAssemblyRef, regen::refTopInstance).
    static regen::AssemblyRef parseAssemblyRef(const regen::AssemblyState& st, const std::string& ref);
    static std::string refTopInstance(const std::string& ref);
    // Mates (core-api §4.1в, assembly.md §7).
    Result<std::string> addMate(const Json& def, const std::optional<std::string>& name = std::nullopt);
    Status updateMate(const std::string& id, const Json& def);
    // «Flip» is available for the mate pair (assembly.md §7.4а): both references have a direction and a side.
    bool mateHasSide(const std::string& type, const std::array<std::string, 2>& refs) const;
    // Current value of a distance / angle mate of the pair at the current placements, in document units (FX9-ASM-002, the
    // fallback type keeps the parts in place); nullopt — the pair has no such mate.
    std::optional<double> mateCurrentValue(const std::string& type, const std::array<std::string, 2>& refs) const;
    // Number of teeth of the gear rim whose pitch cylinder is mate reference ref (assembly.md §7.2, FX10-GEAR-004);
    // nullopt — not resolved or not a pitch cylinder.
    std::optional<int> mateRefTeeth(const std::string& ref) const;
    Status deleteMate(const std::string& id);
    Status renameMate(const std::string& id, const std::string& name);
    Status setMateSuppressed(const std::string& id, bool suppressed);
    // Exploded view (FX7-ASM-005, assembly.md §7д.4) and instance patterns (FX7-ASM-007, §7е.4) of the active assembly.
    Status setExploded(bool active, bool reset = false);
    Result<std::string> addInstancePattern(const Json& def, const std::optional<std::string>& name = std::nullopt);
    Status updateInstancePattern(const std::string& id, const Json& def);
    Status deleteInstancePattern(const std::string& id);
    Status renameInstancePattern(const std::string& id, const std::string& name);
    Status setInstancePatternSuppressed(const std::string& id, bool suppressed);
    // Weld seams (core-api §4.1в, §5 welds; weld.md): assembly objects on instance faces or edges, part geometry unchanged.
    Result<std::string> addWeld(const Json& def, const std::optional<std::string>& name = std::nullopt);
    Status updateWeld(const std::string& id, const Json& def);
    Status deleteWeld(const std::string& id);
    Status renameWeld(const std::string& id, const std::string& name);
    struct WeldInfo {
        std::string id, name, label;
        std::optional<Error> error;  // WELD_BAD_VALUE, WELD_EDGE_LOST, WELD_FACE_LOST, WELD_LINE_NOT_FOUND (weld.md §6)
        double length = 0;  // weld line length, mm (weld.md §6а)
        double volume = 0;  // weld body volume, mm³ (weld.md §6б); 0 — no body
    };
    Result<std::vector<WeldInfo>> welds() const;
    // A point where a hole will be (core-api §4.1в placeFacePoint, FX8-ASM-007): sketch empty — a new sketch on the planar
    // face faceRef with a standalone point at the world point at (one undo step «New sketch»); otherwise a standalone
    // point added to that sketch at the projection of at onto its plane («Add to sketch»). The sketch ID.
    Result<std::string> placeFacePoint(const std::optional<std::string>& sketch, const std::string& faceRef,
                                       const std::array<double, 3>& at);
    // Weld lines and bodies for the 3D view (weld.md §7, FX8-WLD-002): world polylines of the weld line and the body
    // mesh (bodyId "weld:<id>"), welds without a value error, in document order.
    struct WeldDisplay {
        std::string id;
        std::string host;  // top-level instance the weld follows while dragging (the first reference)
        std::vector<std::vector<Point3>> polylines;
        std::optional<io::BodyMesh> body;
    };
    // Welds of the active assembly and of nested assemblies on any depth (FX9-ASM-008); sections — definitions of the
    // view's sections (as instanceSectionTessellation): the body is cut by the effective ones (FX9-WLD-001).
    Result<std::vector<WeldDisplay>> weldDisplay(const kernel::TessParams& params, const std::vector<Json>& sections = {}) const;
    static const Json& weldTypes();  // weld type table (weld.md §2.2)
    struct Placement {
        std::array<double, 3> position{0, 0, 0};
        std::array<double, 4> rotation{1, 0, 0, 0};
    };
    // positions: world placements by instance ID or by path of a free unit of an underdetermined subassembly (its form,
    // assembly.md §2б, FX9-ASM-010).
    Status moveInstances(const std::map<std::string, Placement>& positions);
    // from — the placements of the previous preview of the same drag (FX12-ASM-001, assembly.md §7.6): the solve starts
    // from them instead of the last regeneration, so a long continuous drag follows the current position.
    Result<std::map<std::string, Placement>> previewInstanceDrag(const std::string& id, const Placement& to,
                                                                 const std::map<std::string, Placement>* from = nullptr);
    // "Pull" (FX8-ASM-003, assembly.md §7.6б): point grab of instance id (instance frame) towards the world point target;
    // id — a top-level instance (an underdetermined subassembly — as a whole in its form) or a free unit of an
    // underdetermined subassembly (§2б). The answer — top-level instances and the free units of the system.
    Result<std::map<std::string, Placement>> previewInstancePull(const std::string& id, const Vec3& grab, const Vec3& target);
    // Body of the mate system carrying entry `path` (assembly.md §2б, regen::mateUnitOf): a top-level instance or a free
    // unit of an underdetermined subassembly.
    static std::string mateUnitOf(const regen::AssemblyState& st, const std::string& path);
    // Mate preview (FX8-ASM-001, assembly.md §7.6а): def as for addMate (editing — fields replacing those of that mate).
    struct MatePreview {
        std::map<std::string, Placement> placements;
        std::optional<Error> error, warning;
        std::map<std::string, int> freeDof;  // top-level instances; −1 — not in the system
    };
    Result<MatePreview> previewMate(const Json& def, const std::optional<std::string>& editing = std::nullopt);
    static regen::Motion3 motionOf(const Placement& p) { return regen::motionOf(p.position, p.rotation); }
    // Assembly analysis (core-api §5, assembly.md §7а).
    struct Interference {
        std::string a, b;
        double volume = 0;
    };
    Result<std::vector<Interference>> interferences() const;
    struct InstanceInfo {
        std::string name;
        std::optional<std::string> designation, material;
        std::optional<std::string> library;  // title of the library element (core-api §4.1з)
        double volume = 0;
        std::optional<double> mass;
    };
    Result<InstanceInfo> instanceInfo(const std::string& path) const;

    // Face surface kind by assembly reference "<path>/<name>" (mate type substitution, app-ux §8а); else nullopt.
    std::optional<kernel::SurfaceInfo::Kind> assemblyFaceKind(const std::string& ref) const;
    std::optional<kernel::SurfaceInfo> assemblyFaceSurface(const std::string& ref) const;  // in part coordinates
    regen::AssemblyPtr assembly() const;
    regen::StatePtr assemblyVirtual() const;  // virtual assembly state (assembly.md §7б)
    // Linked files (core-api §4.1в, §5; assembly.md §5а).
    std::vector<std::string> linkChanges() const;
    std::vector<std::pair<std::string, std::optional<Error>>> linkedFiles() const;
    Result<std::vector<std::string>> refreshLinks();
    struct ElementSummary {
        std::string id, name, kind;
    };
    static Result<std::vector<ElementSummary>> fileElements(const std::string& path);  // objects of a document file
    // Library of standard parts (core-api.md §4.1з, library.md): folders in priority order, catalog, search, insertion,
    // update of the document copies, saving a part as a library element.
    void setLibraryDirectories(std::vector<std::string> dirs);
    std::vector<std::string> libraryDirectories() const { return libraryDirs_; }
    static std::vector<std::string> defaultLibraryDirectories();
    void refreshLibrary();
    std::shared_ptr<const library::Catalog> libraryCatalog() const;
    std::vector<size_t> librarySearch(const std::string& query) const;
    // The size row of catalog item `item` found by the query (library.md §5 searchRow); none — nullopt.
    std::optional<std::string> librarySearchRow(size_t item, const std::string& query) const;
    Result<std::string> insertLibraryPart(const std::string& packageId, const std::string& size,
                                          const std::optional<std::array<double, 3>>& position = std::nullopt);
    // Seating on a shaft in one step (library.md §6.5, S6A-BRG-003): the instance and the mates "concentric" (bore —
    // journal) and "coincident" (base face — shoulder); no shoulder — the step next to the journal is found.
    Result<std::string> seatLibraryPart(const std::string& packageId, const std::string& size, const std::string& journal,
                                        const std::optional<std::string>& shoulder = std::nullopt);
    // Insertion by named attachments (library.md §2.4, §6.6, FX10-LIB-003): targets — attachment name → assembly face or
    // vertex; the instance is placed where the attachments already coincide, a mate per attachment, one undo step.
    Result<std::string> attachLibraryPart(const std::string& packageId, const std::string& size,
                                          const std::map<std::string, std::string>& targets);
    // Replacement of a library instance by another element or size keeping its attachments (library.md §6.7, FX10-LIB-004):
    // places — from targets, otherwise the current places of the attachments with the same names.
    Status replaceLibraryPart(const std::string& instance, const std::string& packageId, const std::string& size,
                              const std::map<std::string, std::string>& targets = {});
    // Current places of the attachments of a library instance (library.md §6.7 item 1): attachment name → place.
    std::map<std::string, std::string> libraryAttachmentPlaces(const std::string& instance) const;
    struct LibraryUpdate {
        std::vector<std::string> updated;  // new copy IDs
        std::vector<std::string> missing;  // package IDs not found in the catalog
    };
    Result<LibraryUpdate> updateFromLibrary();
    struct LibrarySaveDef {
        std::string parent;  // library folder (the user folder)
        std::string folder;  // package folder name
        library::Description description;
        std::string sizesText;  // CSV or tab-delimited text (core-api §4.3а)
    };
    Result<std::string> saveLibraryPart(const LibrarySaveDef& def);
    // Languages of the library texts (library.md §17, FX14-I18N-001): the language of the dictionary (the interface
    // language; "ru" — the source texts), reloaded with the library folders. libraryText — the translation of a package
    // text or the text itself; libraryStandardText — a standard code for display. Names of new objects taken from the
    // package texts (instances, connections, element cuts) are in this language; saved names do not change.
    void setLibraryLanguage(const std::string& language);
    const std::string& libraryLanguage() const { return libraryTranslations_.language; }
    const std::string& libraryText(const std::string& text) const { return library::translate(libraryTranslations_, text); }
    std::string libraryStandardText(const std::string& code) const { return library::standardText(code, libraryLanguage()); }
    // Copy of a library package inserted by top-level instance id of the active assembly; nullptr — not a library instance.
    const doc::LibraryPart* instanceLibraryPart(const std::string& id) const;
    // Library of standard methods (core-api.md §4.1и, solutions.md): solution packages in the library folders,
    // the wizard plan with suitable variants, applying a solution as a connection feature, editing, the report.
    std::shared_ptr<const solutions::Catalog> solutionCatalog() const;
    struct SolutionPlan {
        Json report;                    // solutions.md §7.7 (without "changed")
        std::optional<Error> error;     // the plan's first error
        // Per choice: (value, label) of every variant and the values of the suitable ones (solutions.md §8).
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> options;
        std::map<std::string, std::vector<std::string>> suitable;
        std::map<std::string, std::vector<std::string>> unchecked;  // lazy: suitable variants not checked (a new pack measurement)
        std::map<std::string, Json> values;  // effective choice values
    };
    // source — a package ID of the catalog (a new connection) or a connection feature ID of the active assembly (editing).
    // lazy (the wizard, FX8-BLT-001): a variant that needs a new pack measurement is not checked — it is suitable and
    // listed in unchecked (solutions.md §8).
    Result<SolutionPlan> solutionPlan(const std::string& source, const Json& inputs, const Json& choices, bool withOptions = true,
                                      bool lazy = false) const;
    // Choices after choice `changed` got a new value in the wizard (solutions.md §8, FX13-JNT-003): the size choices taken
    // from its table go to their default or the nearest available (solutions::resetDependentChoices); source — as above.
    Result<Json> solutionChoicesAfter(const std::string& source, const Json& choices, const std::string& changed) const;
    // The plan of a description not in the catalog (recording a solution, solutions.md §8а.4): the final state of the
    // active assembly, library tables of the catalog; without options.
    Result<SolutionPlan> solutionDraftPlan(const Json& definition, const Json& inputs, const Json& choices) const;
    // Writes and checks the package parent/folder (solutions.md §8а.3), rebuilds the solution catalog; returns the folder.
    Result<std::string> saveSolution(const std::string& parent, const std::string& folder, const Json& definition);
    // cut — where to cut (solutions.md §7.10): "assembly" (empty — the same) or "parts"; for updateSolution nullopt — unchanged.
    Result<std::string> applySolution(const std::string& packageId, const Json& inputs, const Json& choices, const std::string& cut = {});
    // One connection on several axes (core-api §4.1и, solutions.md §7.11, FX10-WZ-005): axes — assembly references of
    // cylindrical faces for the solution's frame axis input (several — stored as their list, a bolt on each); shared
    // inputs, choices and cut. Empty axes — as applySolution. The ID of the connection (one).
    Result<std::vector<std::string>> applySolutionAxes(const std::string& packageId, const Json& inputs, const std::vector<std::string>& axes,
                                                       const Json& choices, const std::string& cut = {});
    // Editing a connection (core-api §4.1и, FX7-ASM-004): while set, the regeneration evaluates the active assembly with
    // that connection suppressed (its fasteners hidden, parts without its cuts); the document and undo do not change.
    // nullopt — off; also off after any change of the document or of the active element.
    Status setConnectionEditing(const std::optional<std::string>& featureId);
    const std::optional<std::string>& connectionEditing() const { return connectionEditing_; }
    Status updateSolution(const std::string& featureId, const Json& inputs, const Json& choices,
                          const std::optional<std::string>& cut = std::nullopt);
    // Editing with the axes (solutions.md §7.11, FX10-WZ-005): the connection gets all the axes (one — its reference,
    // several — the list), as updateSolution. The ID — the edited one.
    Result<std::vector<std::string>> updateSolutionAxes(const std::string& featureId, const Json& inputs, const std::vector<std::string>& axes,
                                                        const Json& choices, const std::optional<std::string>& cut = std::nullopt);
    // Hole axes for the connection wizard (solutions.md §7.11а, S7F-ASM-008): `ref` — a mate reference giving an axis
    // (a cylindrical face, a circular edge or sketch circle, a reference axis, a line) or a point (a sketch point, a
    // vertex — the hole whose axis passes through it); the answer — assembly references of cylindrical faces of the
    // holes: the picked cylinder itself, otherwise the coaxial one (of the part of preferFace first, a hole before a
    // shaft, the smallest radius, by reference); sameHoles — also every same hole of its part (a hole pattern: the same
    // kind, radius and direction, one face per axis). None — SOL_AXIS_NO_HOLE.
    Result<std::vector<std::string>> holeAxisFaces(const std::string& ref, const std::string& preferFace = {}, bool sameHoles = false) const;
    // Report of a connection in the final state of the active assembly (solutions.md §7.7); nullopt — not evaluated.
    std::optional<Json> connectionReport(const std::string& featureId) const;
    // acceptChordTol in display mesh requests — a ready mesh with chord ≤ acceptChordTol (core-api §5.2, FX3-PERF-002).
    Result<io::BodyMesh> instanceTessellation(const std::string& path, const std::string& bodyId, const kernel::TessParams& params,
                                              double acceptChordTol = 0);
    // In-context design (core-api §4.1в, §5; assembly.md §7в).
    Result<Json> insertPartInContext();
    Status setPartContext(const std::string& part, const Json& context);
    Status freezeContext(const std::string& part, bool frozen);
    Status breakContext(const std::string& part);
    struct ContextInfo {
        std::string assembly, instance;
        bool frozen = false;
        std::vector<std::string> used, features;
    };
    Result<std::optional<ContextInfo>> contextInfo(const std::string& part) const;
    Result<std::vector<io::BodyMesh>> contextTessellation(const kernel::TessParams& params, double acceptChordTol = 0);
    // Assembly sections (core-api §5, sections.md §7а).
    Result<io::BodyMesh> instanceSectionTessellation(const std::string& path, const std::string& bodyId, const kernel::TessParams& params,
                                                     const std::vector<Json>& defs, double acceptChordTol = 0);
    // Drawings: sheets and views of the active drawing (core-api.md §4.1г, drawing.md, S3-DRW-001).
    Result<std::string> addSheet(const Json& def = Json::object());
    Status updateSheet(const std::string& id, const Json& def);
    // Standard and projection method of the active drawing (core-api.md §4.1г, drawing.md §2.5, S3-DRW-008).
    Status updateDrawing(const Json& def);
    Status renameSheet(const std::string& id, const std::string& name);
    Status deleteSheet(const std::string& id);
    Result<std::string> addView(const std::string& sheetId, const Json& def);
    // Three projections in one command (core-api §4.1г, FX1-DRW-001): front view, top view and side view; IDs in order.
    Result<std::vector<std::string>> addThreeViews(const std::string& sheetId, const Json& def);
    Status updateView(const std::string& id, const Json& def);
    // Section list of a view of the active drawing and the main view of its chain (drawing.md §6.26, FX10-DRW-003):
    // found = false — no such view or a broken chain; list — own or inherited (none — the model's enabled sections).
    struct ViewSections {
        bool found = false;
        std::optional<std::vector<std::string>> list;
        std::string baseElement, baseFile;
        bool baseFlat = false;
    };
    ViewSections viewSections(const std::string& viewId) const;
    Status renameView(const std::string& id, const std::string& name);
    Status deleteView(const std::string& id);
    // Active drawing sheet primitives (core-api.md §5, drawing.md §3, §4); a specification has sheet "page<N>" (§4.1е).
    Result<drawing::SheetGraphics> sheetGraphics(const std::string& sheetId) const;
    // Specifications (core-api.md §4.1е, §5; specification.md; S3-SPEC-001).
    Status updateSpecification(const Json& def);
    struct SpecificationInfo {
        drawing::SpecContent content;
        int pages = 1;
    };
    Result<SpecificationInfo> specification(const std::string& element = {}) const;  // empty — active element
    // Sheet sizes of the active drawing (core-api.md §4.1г, §5; drawing.md §6а; S3-DRW-006).
    Result<std::string> addDimension(const std::string& sheetId, const Json& def);
    // The same with a dimension as data (FX7-DRW-003: a dimension of undimensioned() as is; its ID is ignored).
    Result<std::string> addDimension(const std::string& sheetId, const doc::SheetDimension& a);
    Status updateDimension(const std::string& id, const Json& def);
    // Model dimensions onto sheet views (core-api §4.1г, FX1-DRW-003): new dimension IDs (empty — none fit, no step).
    // views (FX7-DRW-009): only onto these views of the sheet; empty — onto the connected views of each model.
    // unplaced (FX8-DRW-002): the model dimensions no view shows (drawing.md §6а.10 item 8).
    Result<std::vector<std::string>> addModelDimensions(const std::string& sheetId, const std::vector<std::string>& views = {},
                                                        std::vector<drawing::UnplacedDimension>* unplaced = nullptr);
    // Model dimensions not yet on the drawing (FX7-DRW-003, core-api §5): what addModelDimensions would add, with
    // primitives; the document does not change. views (FX14-DRW-001): the selected views first, as addModelDimensions.
    Result<std::vector<drawing::DimensionPreview>> undimensioned(const std::string& sheetId, const std::vector<std::string>& views = {}) const;
    Status deleteDimension(const std::string& id);
    // Texts on the sheets of the active drawing (core-api §4.1г, FX7-DRW-006).
    Result<std::string> addSheetText(const std::string& sheetId, const Json& def);
    Status updateSheetText(const std::string& id, const Json& def);
    Status deleteSheetText(const std::string& id);
    // Constructions on the sheet views of the active drawing (core-api §4.1г, drawing.md §6.29, FX14-DRW-006): thin lines
    // bound to the view model; deletion removes the dimensions on the construction too — one undo step.
    Result<std::string> addConstruction(const std::string& sheetId, const Json& def);
    Status updateConstruction(const std::string& id, const Json& def);
    Status deleteConstruction(const std::string& id);
    // The construction build without a document change (placement preview in the client; drawing.md §6.29).
    Result<drawing::ConstructionPreview> previewConstruction(const std::string& sheetId, const doc::SheetConstruction& c) const;
    // Line type of a construction kind without the line field (drawing.md §6.29): dashDot | solid.
    static std::string defaultConstructionLine(const std::string& kind) { return drawing::defaultConstructionLine(kind); }
    // Sheet marks of the active drawing (core-api §4.1г, drawing.md §6.25, FX9-DRW-008): the model annotations, rest
    // roughness and welds onto the sheet views in one undo step (empty — nothing to add, no step); the sign position;
    // deletion (a model mark is dismissed — the command does not bring it back).
    // views (FX14-DRW-005): the selected views first (drawing.md §6.18 «Выбранный вид и место»).
    Result<std::vector<std::string>> addModelMarks(const std::string& sheetId, const std::vector<std::string>& views = {});
    // Model marks not on the drawing with their primitives (FX12-DRW-004, drawing.md §6.25 «Непоставленные»): what
    // addModelMarks would add, no document change; addModelMark — one of them (by its key), step undo.modelMarks.
    Result<std::vector<drawing::MarkPreview>> unplacedMarks(const std::string& sheetId, const std::vector<std::string>& views = {}) const;
    Result<std::string> addModelMark(const std::string& sheetId, const doc::SheetMark& mark);
    // The rest roughness set in the drawing (FX12-DRW-007, drawing.md §6.18а «Заданная в чертеже»): def — the record of
    // the part property (sign, parameter, value); null — the drawing value removed (the model's acts again). One step.
    Status setDrawingRestRoughness(const std::string& sheetId, const Json& def);
    // Annotation set in the drawing itself (FX9-DRW-009): {kind: "annotation", view, def: {type, face, …}, position?}.
    Result<std::string> addMark(const std::string& sheetId, const Json& def);
    Status updateMark(const std::string& id, const Json& def);
    Status deleteMark(const std::string& id);
    // Name template of a drawing named by its first view (drawing.md §2.6, FX9-DRW-003): "{name}" — the model name; the
    // client passes it in the interface language.
    void setDrawingNameTemplate(const std::string& t) { drawingNameTemplate_ = t; }
    // Label format of cut plan parts in the interface language (drawing.md §2.7, FX11-CUT-002): templates with {material}
    // and {thickness}, the decimal separator.
    void setCutLabelFormat(const std::string& withMaterial, const std::string& withoutMaterial, const std::string& decimalSeparator);
    Status prepareViewPlane(Json& view, const Json& def) const;  // FX7-DRW-007: orientation "plane" of a main view
    Result<std::vector<drawing::ViewEntity>> viewEntities(const std::string& sheetId, const std::string& viewId) const;
    Result<drawing::DimensionPreview> previewDimension(const std::string& sheetId, const doc::SheetDimension& a,
                                                       const std::optional<Point2>& cursor) const;
    // Sections (core-api.md §4.1б, S2-VIEW-002, S2-VIEW-005).
    Result<std::string> addSection(const Json& def, const std::optional<std::string>& name = std::nullopt);
    Status updateSection(const std::string& id, const Json& def);
    Status deleteSection(const std::string& id);
    Status renameSection(const std::string& id, const std::string& name);
    Status setSectionHidden(const std::string& id, bool hidden);
    std::string activeElement() const { return doc().active().id; }
    Status setSuppressed(const std::string& id, bool value);
    Status moveFeature(const std::string& id, int newIndex);
    // Moving a block of features (core-api.md §4.4, S2-FEAT-010).
    Status moveFeatures(const std::vector<std::string>& ids, int newIndex);
    Status setRollback(const doc::Rollback& r);

    // §4.5
    Result<PreviewId> previewFeature(const Json& definition, const std::optional<std::string>& editing = std::nullopt,
                                     const std::vector<Json>& aux = {});
    void cancelPreview();

    // §4.6
    Result<std::string> createSketch(const Json& plane, const std::vector<Json>& aux = {});
    Status enterSketch(const std::string& featureId);
    Status exitSketch();
    // «Undo» (core-api §4.6, S7F-SK-004): the open sketch's data back to what it was when it was opened
    // (createSketch / enterSketch), one undo step if anything changed; then the context is closed.
    Status sketchCancel();
    // The open sketch differs from what it was when it was opened (the sketchCancel comparison; FX10-SK-001).
    bool sketchChanged() const;
    Result<std::map<std::string, std::string>> sketchAdd(const Json& items);
    Status sketchUpdate(const Json& changes);
    Status sketchDelete(const std::vector<std::string>& ids);
    // Displace an auto-constraint by explicit added ones (core-api §4.6, S1-SK-016): removed ID or "" — no candidates.
    Result<std::string> sketchYieldAuto(const std::vector<std::string>& added);
    // Fully define the open sketch (core-api.md §4.6.2, S2-SK-011, FX4-SK-001, FX4-SK-002): IDs of the added
    // constraints; layout — label sizes for the layout (mm; 0 — from the sketch extent).
    // splines = false — «Dimension main geometry» (FX7-SK-013): weak dimensions of spline
    // poles and weights stay weak.
    Result<std::vector<std::string>> sketchFullyDefine(const FullyDefineLayout& layout = {}, bool splines = true);
    // Weak dimension (relation) of the open sketch -> regular one (core-api §4.6.2, S4-WEAK-002): value — new value
    // (expression; absent — current), label — label position. Answer — ID of the new constraint.
    Result<std::string> sketchStrengthen(const std::string& weakId, const std::optional<std::string>& value = std::nullopt,
                                         const std::optional<Point2>& label = std::nullopt);
    // Fillet and chamfer of the corner of two segments with a virtual vertex (core-api.md §4.6.2, S2-SK-001, -002).
    Result<std::map<std::string, std::string>> sketchFillet(const std::string& point, const std::string& radius);
    Result<std::map<std::string, std::string>> sketchChamfer(const std::string& point, const std::string& d1,
                                                             const std::string& second, bool secondIsAngle);
    Result<std::map<std::string, std::string>> sketchCorner(const std::string& point, const std::string& v1, const std::string& v2,
                                                            bool v2Angle, bool fillet);
    Result<TrimResult> sketchTrim(const std::string& curve, const Point2& pick);
    // Simplification without changing the drawing (core-api §4.6.2, FX7-SK-001): a curve lying exactly on the same path
    // as curve (one of them construction) — the extra construction one is deleted and its constraints and dimensions
    // move onto the other one. removed/kept — empty: no duplicate; refused — the move would change the geometry or
    // the dimensions (nothing is changed).
    struct DuplicateMerge {
        std::string removed, kept;
        bool refused = false;
    };
    Result<DuplicateMerge> sketchMergeDuplicate(const std::string& curve);
    // Linked sketch groups (core-api.md §4.6.2, sketch.md §2.6; S4-SKT-001, -002, -005).
    Result<std::string> sketchGroupCreate(const std::string& type, const std::vector<std::string>& sources, const Json& params);
    Status sketchGroupUpdate(const std::string& id, const std::vector<std::string>& sources, const Json& params);
    Status sketchGroupUnlink(const std::string& id);
    // Weak dimension anchor point of its loop (core-api §4.6.2, S4-WEAK-006): the point goes first in weakAnchors.
    Status sketchSetWeakAnchor(const std::string& point);
    Status sketchDragBegin(const std::string& entityId, const Point2& grabPoint);
    Result<SketchState> sketchDragUpdate(const Point2& targetPoint);
    Status sketchDragEnd(bool commit);

    // §5
    DocumentInfo document() const;
    PartResultPtr regenResult() const;
    Result<FeatureStatusInfo> featureStatus(const std::string& id) const;
    // «Select all adjacent faces» of the draft (core-api §5, FX10-PT-005): full names of the faces sharing an edge with a
    // seed — the neutral face, the planar faces lying in the neutral plane, the given faces — except the seeds and the
    // planar faces parallel to the neutral plane; in name-table order.
    Result<std::vector<std::string>> draftAdjacentFaces(const Json& neutral, const std::vector<std::string>& faces, const StateRef& ref) const;
    std::map<std::string, int> regenStats() const;
    // Kernel computations of drawing views since the session start (core-api §5, FX8-DRW-001): "projections", "sections".
    std::map<std::string, int> drawingProjectionStats() const;
    std::map<std::string, double> regenTimings() const;
    // Number of display meshes computed (mesh cache misses) since session start (core-api §5, FX3-PERF-002).
    int tessellationCount() const { return tessellations_; }
    EditContext editContext() const { return {ctx_}; }
    Result<SketchState> sketchState(const std::string& featureId) const;
    // Geometry of projections of sources (edges, body vertices, sketch axes — as the source of a projected entity) onto
    // the sketch plane without changing the document (core-api §5, FX3-SK-001: dimension preview); nullopt — none.
    Result<std::vector<std::optional<sketch::Projection>>> projectionPreview(const std::string& sketchId,
                                                                             const std::vector<std::string>& sources) const;
    // Closed contour of body edges that contains edge, seen from the sketch plane (core-api §5, sketch.md §2.6.5,
    // S7F-SK-005): edge names in traversal order; empty — not in a closed contour.
    Result<std::vector<std::string>> sketchEdgeContour(const std::string& sketchId, const std::string& edge) const;
    // Implicit body targets of a sketch (core-api §5, sketch.md §2.5): projections of edges and vertices off the sketch
    // plane (FX8-SK-010), silhouette lines and meridian extremes of bodies of revolution (FX8-SK-002) in the state before
    // the sketch; cached per state and plane.
    Result<std::shared_ptr<const std::vector<regen::BodyTarget>>> sketchBodyTargets(const std::string& sketchId) const;
    Result<std::vector<BodyInfo>> bodies(const StateRef& ref) const;
    // Flat pattern of the sheet metal part in the final state (core-api.md §5, sheet.md §2.4, S5-FLAT-001).
    Result<sheet::FlatPattern> flatPattern() const;
    // Reference planes and axes of a state (core-api.md §5, S2-REF-*).
    struct ReferenceGeometry {
        std::map<std::string, Plane> planes;
        std::map<std::string, regen::RefAxis> axes;
    };
    Result<ReferenceGeometry> referenceGeometry(const StateRef& ref) const;
    // Reference planes, axes and visible sketches of the nested subassemblies of the active assembly in world placement
    // (core-api.md §5, assembly.md §2 «Assembly objects», FX9-ASM-008); empty — not an assembly or none.
    std::vector<regen::AssemblyDatum> nestedReferenceGeometry() const;
    Result<io::BodyMesh> tessellation(const std::string& bodyId, const StateRef& ref, const kernel::TessParams& params,
                                      double acceptChordTol = 0);
    Result<TopologyInfo> topologyInfo(const std::string& name, const StateRef& ref) const;
    // Measurement of one or two entities (core-api.md §5 measure, FX10-MEAS-001): stable names of faces, edges and
    // vertices of the state, source and reference planes.
    Result<std::vector<MeasureItem>> measure(const std::vector<std::string>& refs, const StateRef& ref) const;
    // Selection parsing (core-api.md §5, selection.md §7.1, FX7-SEL-001): input — plane | direction | axis | point;
    // picks — references of the picked elements in click order.
    Result<SelectionResult> selectionCandidates(const std::string& input, const std::vector<std::string>& picks, const StateRef& ref,
                                                const SelectionFilter& filter = {}) const;
    Result<RegionMesh> regionMesh(const std::string& sketchFeatureId, const std::string& regionId, double chordTol) const;
    Result<ExpressionCheck> checkExpression(const std::string& text, std::optional<expr::Dim> expected) const;
    // Queries the client uses instead of calling kernel modules directly (architecture.md §2).
    expr::DocumentUnits units() const;
    expr::VariableTable variableValues() const;
    int variableReferenceCount(const std::string& variableId) const;
    Result<Json> featureDefinition(const std::string& featureId) const;
    // Lost references of the active element's features with their ghosts (core-api §5, regen.md §8а, S7F-FEAT-003).
    Result<std::vector<LostReference>> lostReferences(const kernel::TessParams& params) const;
    // Number of steps that can be undone (core-api §4.2): the client's undo point of the repair wizard (app-ux §5.5в).
    size_t undoDepth() const;
    Result<sketch::TrimPlan> sketchTrimPreview(const std::string& curve, const Point2& pick) const;
    static std::optional<Plane> datumPlane(const std::string& datumId);
    kernel::TessParams stlDefaultTolerances() const;
    // Sketches shown in a part (app-ux §3.4): not suppressed and not used by features (extrude
    // or revolve profile), in history order.
    std::vector<std::string> visibleSketches() const;
    // Hole positions of a sketch (core-api.md §5 holePoints, regen.md §4.13 item 1): IDs of representative points.
    Result<std::vector<std::string>> holePoints(const std::string& sketchFeatureId) const;
    // Material of the state's bodies on one side of the profile (core-api.md §5 extrudeSideMaterial, FX1-FEAT-003).
    enum class SideMaterial { None, Partial, Full };
    Result<SideMaterial> extrudeSideMaterial(const std::string& sketchFeatureId, const Json& profiles, bool reverse,
                                             const StateRef& ref) const;
    // Thread fit for a cylindrical face (core-api.md §5 threadFit, FX2-FEAT-001).
    Result<ThreadFit> threadFit(const std::string& face, const StateRef& ref) const;
    // Hole table (core-api.md §5, S2-FEAT-008).
    static const Json& holeTables();
    // A feature type with a tool — a source of patterns and mirrors (core-api.md §5 isToolFeature, FX7-PT-008).
    static bool isToolFeature(doc::FeatureType t) { return doc::isToolFeature(t); }
    static const Json& threadTables();
    static const Json& materialLibrary();
    static std::vector<std::string> fonts();
    static std::string defaultFont();  // font.md §2  // core-api §5, S2-SK-013
    // Font library (core-api §5, font.md §2, §2а; S3-FONT-001, S3-SET-001).
    static std::string builtinDefaultFont();
    static void setPreferredFont(const std::string& name);
    static std::vector<std::string> defaultFontDirectories();
    static void setFontDirectories(std::vector<std::string> dirs);
    static std::string defaultDrawingFont(font::DrawingStandard s);
    static void setPreferredDrawingFont(font::DrawingStandard s, const std::string& name);  // drawing.md §3.2, S3-DRW-008
    // Mass properties of the active element (core-api.md §5, S2-PART-003).
    struct MassInfo {
        double volume = 0, area = 0;
        Point3 centroid = Point3::Zero();
        std::array<double, 3> principalMoments{0, 0, 0};  // kg·mm² at density 1 kg/m³, ascending
        std::array<Vec3, 3> principalAxes{Vec3::UnitX(), Vec3::UnitY(), Vec3::UnitZ()};
        std::optional<double> density, mass;  // kg/m³, kg
    };
    Result<MassInfo> massProperties(const StateRef& ref) const;
    struct AssemblyMass {  // assembly.md §7а
        MassInfo props;
        int missingMaterial = 0;
    };
    Result<AssemblyMass> assemblyMassProperties() const;
    struct FaceColor {
        std::string bodyId, face, color;
    };
    Result<std::vector<FaceColor>> faceColors(const StateRef& ref) const;
    // Sections (core-api.md §5, sections.md): status — "ok", "hidden" or an error.
    struct SectionInfo {
        std::string id, name;
        Json def;
        bool hidden = false;
        std::optional<Error> error;
    };
    std::vector<SectionInfo> sections() const;
    Result<std::optional<Error>> sectionStatus(const Json& def, const StateRef& ref) const;  // nullopt — ok
    Result<Plane> sectionPlane(const Json& def, const StateRef& ref) const;
    Result<io::BodyMesh> sectionTessellation(const std::string& bodyId, const StateRef& ref, const kernel::TessParams& params,
                                             const std::vector<Json>& defs, double acceptChordTol = 0);
    // Technical requirements (core-api.md §4.1д, §5; pmi.md; S3-PMI-001…003).
    Result<std::string> addAnnotation(const Json& def);
    Status updateAnnotation(const std::string& id, const Json& def);
    Status deleteAnnotation(const std::string& id);
    struct AnnotationInfo {
        std::string id;
        Json def;
        std::optional<Error> error;  // status: nullopt — OK
        std::string bodyId, face;    // resolved face (full name); empty — none
        std::optional<Point3> anchor;
        std::optional<Vec3> normal;
        std::optional<double> nominal, upper, lower;  // mm (fit)
        // Assembly on the exploded view (FX14-PMI-001): the displacement of the face's instance by the explosion, already
        // applied to `anchor` and `normal`; `at` and `labelOffset` of `def` stay assembled — the shown label offset is
        // `move.R · labelOffset`, a shown point goes back by the inverse. Identity otherwise.
        regen::Motion3 move;
    };
    Result<std::vector<AnnotationInfo>> annotations(const StateRef& ref) const;
    static Result<pmi::Deviations> fitDeviations(const std::string& field, double nominal) { return pmi::fieldDeviations(field, nominal); }
    // Tolerance written in a dimension field (pmi.md §5.4, FX12-PMI-004): nullopt — no tolerance; PMI_FIELD_SYNTAX.
    static Result<std::optional<Json>> parseTolerance(const std::string& text) { return pmi::parseToleranceText(text); }
    // Tolerance label in the part and the assembly — a field with its deviations (pmi.md §5.2, FX13-TOL-002).
    static std::string toleranceLabel(const Json& fit, const std::optional<pmi::Deviations>& d) { return pmi::toleranceLabel(fit, d); }
    // Standard fields of the tolerance field list (pmi.md §4.3а, FX13-TOL-001): hole or shaft, frequent first.
    static const std::vector<std::string>& standardFields(bool hole) { return hole ? pmi::STANDARD_HOLE_FIELDS : pmi::STANDARD_SHAFT_FIELDS; }
    // The annotation is a node of the «Annotations» tree (app-ux §4.2в, FX13-TOL-003): a sketch dimension tolerance is
    // part of the dimension, not a node.
    static bool annotationInTree(const Json& a) { return !(a.value("type", std::string()) == "fit" && a.contains("dimension")); }
    // Threads of a state (core-api.md §5, S2-FEAT-009).
    Result<std::vector<regen::ThreadEntry>> threads(const StateRef& ref) const;
    // Gear rims of a state (core-api.md §5 gears, regen.md §4.17, S6-GEAR-002).
    Result<std::vector<regen::GearEntry>> gears(const StateRef& ref) const;
    // Gear geometry by parameters (core-api.md §5 gearGeometry, gear.md §3): diameters, warnings or an error.
    static Result<gear::Geometry> gearGeometry(double module, int teeth, double shift, bool internal) {
        return gear::geometry({module, teeth, shift, internal});
    }

    // For tests and tools (§3).
    bool waitIdle(int timeoutMs);
    // Delay before every regeneration in the worker thread (§3, FX14-QA-001): reproduces a slow regeneration under load.
    void setRegenDelayForTest(int ms) { regenDelayMs_.store(ms); }
    uint64_t revision() const { return revision_; }

private:

    const doc::Document& doc() const { return undo_->current().document; }
    Status checkNoDrag() const;
    Status checkMateDefinition(const Json& def) const;  // core-api §4.1в
    Status checkAssemblyFeature(const Json& def) const;  // assembly feature definition (assembly.md §7г.5)
    Status fillMate(doc::Mate& m, const Json& def);
    // Weld record check for addWeld/updateWeld (core-api §4.1в): document with the record, or API_INVALID_DEFINITION.
    Result<doc::Document> withWeld(const doc::Document& base, const Json& weld) const;
    // The document with connection featureId changed (updateSolution, S7F-ASM-008): not materialized, not committed.
    Result<doc::Document> updatedSolution(const std::string& featureId, const Json& inputs, const Json& choices,
                                          const std::optional<std::string>& cut);
    // An assembly sketch on a face with standalone points (solutions.md §7.12, FX8-BLT-002), added to nd; its ID.
    Result<std::string> pointsSketch(doc::Document& nd, const std::string& faceRef, const Json& points) const;
    Result<std::optional<std::vector<Point3>>> documentSketchPoints(const doc::Element& el, const solutions::Definition& def,
                                                                    const Json& inputs) const;
    // Frame of a sketch on the face faceRef of the final state (sketch.md §6).
    Result<Plane> faceSketchFrame(const std::string& faceRef) const;
    // Instance pattern record check for addInstancePattern/updateInstancePattern (core-api §4.1в).
    Result<doc::Document> withInstancePattern(const doc::Document& base, const Json& pattern) const;
    Result<BBox3> modelBox(const std::string& file, const std::string& element);  // view model extent (addThreeViews)
    // subject — ID of the feature/variable whose name becomes params.name of the step title (core-api §2); empty — none
    Status commit(doc::Document nd, const std::string& labelKey, std::vector<std::string> touchedSketches = {},
                  const std::string& subject = {});
    void resolveSketches(doc::Document& nd, const doc::Document& before, const std::vector<std::string>& touched);
    // diskCache — opening a document: entries from the disk cache before regeneration, and file write after (§4.1ж).
    void scheduleRegen(bool clearCache = false, bool diskCache = false);
    void scheduleCacheStore();  // write the disk cache for the saved content (§4.1ж)
    void publish(PartResultPtr r, std::shared_ptr<const regen::CacheEntries> entries = nullptr,
                 std::map<std::string, regen::StatePtr> ghosts = {});
    Result<regen::StatePtr> stateOf(const StateRef& ref) const;
    Result<Plane> sketchFrame(const doc::Feature& f) const;
    // Sketch data with projection geometry (core-api §4.6, Р-018); nullopt — projections are needed but the state
    // "before the sketch" is not computed yet. d — the document in which the sketch position is looked up.
    std::optional<sketch::SketchData> withProjections(const doc::Document& d, const doc::Feature& f) const;
    std::optional<sketch::SketchData> withProjections(const doc::Document& d, const doc::Feature& f, sketch::SketchData sd) const;
    void reset(doc::Document d, bool fromFile = false);
    // One attempt to build the reconstructed part in a temp session (reconstruct_commands.cpp, reconstruction.md §5).
    // from — document the part is added to (at position position); base — base body (absent — without it), first —
    // first operation of the plan; skipOps — operations not built (blocks kept in the base body, §5.5 item 2); failedOp —
    // index of the failed operation (otherwise SIZE_MAX).
    std::optional<doc::Document> buildReconstruction(const doc::Document& from, const std::string& bodyId, int position, const std::string& name,
                                                     const rec::Plan& plan, const std::optional<kernel::Shape>& base, size_t first,
                                                     const std::set<size_t>& skipOps, bool verify, const kernel::Shape& source,
                                                     double roundTol, ReconstructReport& report, size_t& failedOp,
                                                     const Cancel* cancel = nullptr) const;
    // Reconstruction of one solid (§5.5: attempts and rollback) — new part in document from; nullopt — failure.
    // Replacing a body by its reconstruction (core-api §4.7г, FX10-IO-008): mate references of the assemblies of d on the
    // body's faces, edges and vertices move to the coinciding ones of `result` (a part or the result assembly); d still
    // has the body and its instances. remapped — references moved; lost — (assembly, mate) left as they were.
    Status remapBodyMateRefs(doc::Document& d, const std::string& bodyId, const std::string& result, double tol, int& remapped,
                             std::vector<std::pair<std::string, std::string>>& lost, const Cancel* cancel) const;
    std::optional<doc::Document> reconstructSolid(const doc::Document& from, const std::string& bodyId, int position, const std::string& name,
                                                  const kernel::Shape& solid, double roundTol, ReconstructReport& report,
                                                  const Cancel* cancel = nullptr, bool importOnly = false) const;
    // "Dimension everything" with value text valueText(value in internal units, is angle?) (reconstruction.md §5.4).
    Result<std::vector<std::string>> fullyDefineWith(const FullyDefineLayout& layout,
                                                     const std::function<std::string(double, bool)>& valueText, bool splines = true);

    std::unique_ptr<kernel::Kernel> ownedKernel_;  // before k_: destroyed after the worker thread and regen_
    const kernel::Kernel& k_;
    Callbacks cb_;
    std::unique_ptr<doc::UndoStack> undo_;
    std::optional<std::string> path_;
    std::optional<std::string> ctx_;
    std::optional<doc::Feature> sketchEntry_;  // the open sketch as it was opened (sketchCancel, S7F-SK-004)
    uint64_t revision_ = 0;
    std::optional<std::string> connectionEditing_;  // setConnectionEditing (FX7-ASM-004)
    doc::Document regenDocument() const;  // doc() with the edited connection suppressed
    int groupDepth_ = 0;
    std::string groupLabel_;
    bool groupPushed_ = false;

    // Result of the last synchronous solve of the open sketch.
    std::optional<SketchState> openSketch_;

    // Dragging.
    struct Drag {
        sketch::SketchData data;  // with projections (core-api §4.6)
        sketch::DragPlan plan;
        sketch::SolvedState state;
        std::map<std::string, double> reference;
    };
    std::optional<Drag> drag_;

    // Worker thread.
    regen::Regenerator regen_;
    mutable std::mutex mu_;
    mutable std::pair<const regen::PartState*, regen::StatePtr> flatCache_;  // flat pattern state of the final state (§5.1)
    // sketchBodyTargets cache (FX8-SK-002): the state it was computed for (held), the plane, the targets.
    struct BodyTargetsCache {
        regen::StatePtr state;
        Plane frame;
        std::shared_ptr<const std::vector<regen::BodyTarget>> targets;
    };
    mutable BodyTargetsCache bodyTargetsCache_;
    std::shared_ptr<Cancel> regenCancel_;
    std::shared_ptr<Cancel> previewCancel_;
    PartResultPtr published_;
    // Reference states of features (regen.md §8а): context state of the last published result without REGEN_REF_LOST.
    std::map<std::string, regen::StatePtr> ghostStates_;  // under mu_
    std::map<std::string, int> stats_;
    std::map<std::string, double> timings_;
    // Disk cache (§4.1ж): directory, limit; entries used by the published regeneration (under mu_).
    std::string cacheDir_;
    uint64_t cacheMaxBytes_ = REGEN_DISK_CACHE_MAX_BYTES;
    std::atomic<int> regenDelayMs_{0};  // setRegenDelayForTest; initial — CONSTRUE_TEST_REGEN_DELAY_MS (§3)
    std::shared_ptr<const regen::CacheEntries> publishedEntries_;
    uint64_t currentRevision_ = 0;  // for threads (under mu_)
    int nextPreview_ = 1;
    int nextExport_ = 1;
    PreviewId previewId_;
    struct PreviewData {
        PreviewId id;
        regen::StatePtr state;
        regen::StatePtr tool;
        std::map<std::string, regen::StatePtr> after;
    };
    std::shared_ptr<const PreviewData> preview_;

    // Tessellation cache (§5.2).
    TessCache tessCache_{TESS_CACHE_MAX_ENTRIES};
    // Drawing view models and projections (§5.4): models — from the last published result that has them (under mu_).
    mutable ProjectionCache projCache_;  // thread-safe; filled from const queries too, not changing observable state
    std::shared_ptr<const std::map<std::string, regen::DrawingModel>> drawingModels_;
    // Worker thread: all active drawing sheets, computing missing projections into the cache (§5.4); false — cancelled.
    bool precomputeDrawing(const doc::Document& d, const regen::PartResult& r, const std::string& self, const Cancel* c);
    // Meshes of sectioned bodies: key without chord → chord → mesh (ready mesh — as in tessCache_, §5.2).
    std::map<std::string, std::map<double, io::BodyMesh>> sectionCache_;
    std::optional<io::BodyMesh> sectionCached(const std::string& key, double chordTol, double acceptChordTol) const;
    void sectionPut(const std::string& key, double chordTol, const io::BodyMesh& m);
    int tessellations_ = 0;
    // Linked files (assembly.md §5а).
    struct LinkStat {
        bool exists = false;
        uintmax_t size = 0;
        long long mtime = 0;
    };
    regen::LinkMap links_;
    std::string drawingNameTemplate_ = "Чертёж {name}";  // drawing.md §2.6
    void autoNameDrawing(doc::Document& nd, const std::string& file, const std::string& element) const;
    std::map<std::string, LinkStat> linkStats_;
    // root — document from which link traversal starts (absent — the session's current document).
    void loadLinks(const doc::Document* root = nullptr);
    void syncAssemblyPositions();  // solved instance positions → document (assembly.md §7.5)
    // Form of the instance `node` of an underdetermined subassembly (assembly.md §2б): its free units in state st.
    static std::vector<doc::Instance::FormPlacement> instanceForm(const regen::AssemblyState& st, const regen::InstanceEntry& node);
    // Owned cuts of connections "in parts" (solutions.md §7.10): the listed connections of the active assembly of nd
    // materialized in the parts of nd (commands, item 5а); from the published result of the current revision — the
    // current snapshot amended (item 5б); the linked part files written on saving (item 6); the edit lock (item 2).
    void materializeConnections(doc::Document& nd, const std::vector<std::string>& connections) const;
    void syncOwnedCutsFromPublished();
    Status writeLinkedOwnedCuts(const std::string& newPath, const std::optional<std::string>& oldPath);
    Status checkOwnedFeature(const std::string& id, bool deleting) const;
    Status rebindAssemblyObject(const std::string& id, const std::vector<Rebind>& replacements);  // mate or weld
    // Owned features aux (with their allocated ids) inserted at pos of nd before their consumer owner (core-api §4.4).
    Status insertAux(doc::Document& nd, const std::vector<Json>& aux, const std::vector<std::string>& ids, const std::string& owner, int pos);
    // Object reference of insertInstance / replaceInstance / replaceView (core-api §4.1в): relative file path, element,
    // its name and kind; checkCycle — the active assembly must not be reachable from the object.
    struct ResolvedRef {
        std::string file, element, name, kind;
    };
    Result<ResolvedRef> resolveRef(const Json& ref, bool checkCycle) const;
    // Name (by base), fixation and position of a new instance of assembly el (insertInstance, insertLibraryPart).
    // attachLibraryPart with the error codes and undo step of seatLibraryPart when `seat` (library.md §6.5).
    // replace — the instance replaced (§6.7) instead of a new one.
    Result<std::string> attachLibraryPartImpl(const std::string& packageId, const std::string& size,
                                              const std::map<std::string, std::string>& targets, bool seat,
                                              const std::string& replace = {});
    // Attachment mates of a library instance (§6.7 item 1): attachment name → (mate ID, place).
    std::map<std::string, std::pair<std::string, std::string>> attachmentMates(const std::string& instance) const;
    void placeNewInstance(const doc::Element& el, doc::Instance& in, const std::string& base,
                          const std::optional<std::array<double, 3>>& position) const;
    std::vector<std::string> libraryDirs_;
    library::Translations libraryTranslations_{"ru", {}};  // library.md §17
    mutable std::shared_ptr<const library::Catalog> catalog_;  // scanned on first use (library.md §4)
    // The plan of a solution description over the state ref with the given tables (solutionPlan, solutionDraftPlan).
    // The definition, tables, library copies and state of a wizard source (a package of the catalog or a connection).
    Status solutionSource(const std::string& source, doc::Json& definition, solutions::TableProvider& tables,
                          std::map<std::string, std::string>& libraries, StateRef& ref) const;
    Result<SolutionPlan> planOf(const Json& definition, const solutions::TableProvider& tables,
                                const std::map<std::string, std::string>& libraries, StateRef ref, const Json& inputs,
                                const Json& choices, bool withOptions, bool lazy = false) const;
    mutable std::shared_ptr<const solutions::Catalog> solutionCatalog_;
    // Weld lines and bodies of the virtual assembly state (weld.md §6а, §6б): key — the record and the leg value.
    struct WeldGeom;
    // d — document of the weld record (values, weld.md §6 item 1); null — the session document.
    std::shared_ptr<const WeldGeom> weldGeom(const regen::StatePtr& v, const Json& weld, const doc::Document* d = nullptr) const;
    // Welds acting in the active assembly (weld.md §7, FX9-ASM-008): its own and those of the nested assemblies (not
    // suppressed, not hidden) with references prefixed by the path of the nested instance; in instance order.
    std::vector<ShownWeld> shownWelds(const regen::AssemblyState& a) const;
    static void installWeldModelBodies();  // weld bodies in drawing view models (FX9-WLD-002), once per process
    Status turnForMate(doc::Document& nd, const doc::Mate& m);  // «Flip» (FX9-ASM-005)
    // Weld bodies of the shown welds for the assembly mass (weld.md §7а, FX9-WLD-002): host part element (the instance of
    // the first reference; null — unknown) and world mass properties at density 1.
    std::vector<std::pair<const doc::Element*, kernel::MassProperties>> weldMasses(const regen::AssemblyState& a) const;
    // Common part of the effective sections of defs for top-level instance top in world coordinates (sections.md §7а);
    // nullopt — no effective section.
    std::optional<kernel::Shape> assemblyCutAway(const regen::AssemblyState& a, const std::vector<Json>& defs, const std::string& top) const;
    // Pack measurements of the wizard plans (solutions.md §8, FX8-BLT-001): one state at a time.
    mutable std::mutex packMu_;
    mutable regen::StatePtr packState_;
    mutable regen::PackCache packCache_;
    mutable std::mutex weldMu_;
    mutable regen::StatePtr weldState_;
    mutable std::map<std::string, std::shared_ptr<const WeldGeom>> weldCache_;  // the same folders (solutions.md §4)
    const doc::Element* elementOfEntry(const regen::InstanceEntry& e) const;  // instance element (own or linked)
    std::string selfPath() const;
    // Specification (specification_commands.cpp): linked documents and sheet/fields of the active specification.
    drawing::LinkedDocs specLinkedDocs() const;
    // Final state of the specification's assembly from the published regeneration — its connection reports for the
    // fastener rows (specification.md §3.1 item 2а, core-api §5 `specification`, FX14-SPEC-001); none — nullptr.
    regen::StatePtr specAssemblyState(const doc::Element& spec) const;
    // Specification rows with the fastener rows of the published regeneration.
    drawing::SpecContent specContent(const doc::Element& spec) const;
    Result<drawing::SheetGraphics> specificationSheet(const std::string& pageId) const;
    Result<std::map<std::string, std::string>> specificationTitleDefaults(const std::string& pageId) const;
    static LinkStat statOf(const std::string& abs);  // sections.md §7 (cleared entirely on overflow)
    // Export pipeline (core-api §4.7и, S7F-RF-013). Copy of the document with `element` active (empty — the active one);
    // no such element — API_NOT_FOUND.
    Result<doc::Document> exportDocument(const std::string& element) const;
    // Synchronous result of d's active element at the current revision: the published one when it is the session's
    // active element and the revision matches, otherwise a separate regeneration (REGEN_CANCELLED on failure).
    Result<PartResultPtr> elementResult(const doc::Document& d);
    // Export job over a snapshot of the current revision: write gets the result of d's active element (published or a
    // separate regeneration in the worker thread) and the snapshot's file references, and returns the export status,
    // reported by onExportFinished.
    using ExportWrite = std::function<Status(const doc::Document& d, const regen::PartResult& r, const regen::LinkMap& links)>;
    ExportId enqueueExport(doc::Document d, ExportWrite write);
    // Export job without a regeneration (file writing of prepared data); its status — to onExportFinished.
    ExportId enqueueExportJob(std::function<Status(const Cancel*)> run);
    // Worker thread (§3) — last: stopped in the session destructor before the other fields are destroyed.
    JobRunner runner_;
};

// Mesh of a body in the §5.2 layout.
Result<io::BodyMesh> bodyMesh(const kernel::Kernel& k, const regen::PartState& st, int bodyIndex,
                              const kernel::TessParams& params, const Cancel* cancel = nullptr);
// Reference states of features of the active element (regen.md §8а): feature ID → its context state in r, for every
// evaluated feature without REGEN_REF_LOST.
std::map<std::string, regen::StatePtr> referenceStates(const doc::Document& d, const regen::PartResult& r);
// The references lost by a feature result (regen.md §8а): ids of its REGEN_REF_LOST errors, in order, without repeats.
std::vector<std::string> lostReferenceIds(const regen::FeatureResult& fr);
// Bases of the default names in the interface language (document.md §2.12 «Язык основ», FX14-UI-004), for the client:
// the keys of the built-in bases, and the client's table «key → base» for the whole process (a key missing from it or
// with an empty base — the built-in one).
std::vector<std::string> nameBaseKeys();
// The built-in (Russian) table «key → base» itself (core-api.md §4.1).
const std::map<std::string, std::string>& builtinNameBases();
void setNameBases(std::map<std::string, std::string> table);

}  // namespace construe::api
