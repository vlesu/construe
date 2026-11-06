// Drawing dimensions — internal parts of the drawing module (spec/drawing.md §6а): reference geometry, dimension
// construction and dimensions from the model (dimensions.cpp, model_dimensions.cpp). Not a public interface.
#pragma once
#include "drawing/dimensions.h"

#include "base/geom2.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace construe::drawing::dimdetail {

Error badRefs(const char* detail);

// Reference geometry in view coordinates (§6а.2).
struct RefGeom {
    enum class Kind { Point, Segment, Circle, Arc, Curve };
    Kind kind = Kind::Point;
    Point2 a{0, 0}, b{0, 0};  // point — a; segment — a = π(p1), b = π(p2)
    Point2 center{0, 0};
    double radius = 0, start = 0, end = 0;
    Point3 p1 = Point3::Zero(), p2 = Point3::Zero();  // segment: endpoints in world coordinates (dimension in face plane, §6а.4)
    Vec3 dir3 = Vec3::Zero();        // segment: (p2 − p1)/|p2 − p1|
    double depth = 0, depthMin = 0;  // greatest and smallest depth of the points (δ)
    int edgeEntry = -1;              // edge: index in the edge name table (tolerance from the model, §6а.3)
    int faceEntry = -1;              // silhouette and cut: face index in the name table
    bool edgeOn = false;             // segment — circle seen edge-on (axis ⊥ view direction), length — diameter
    double edgeOnRadius = 0;
    double cylRadius = 0;            // cylinder silhouette and cut: surface radius ρ
    double halfWidth = 0;            // cylinder silhouette and cut: line distance from the axis (ρ for silhouette, √(ρ² − g²) for cut)
    int side = 0;
    std::string arc;                 // linear: near | far — circle point instead of the center, extreme point of a curve (§6а.4)
    // Curve (FX7-DRW-010): point set of the kernel (curve edge or meridian of a face of revolution) of body `body`; its point
    // for a dimension — the extreme one along the measurement direction (§6а.4 item 1). Silhouette: surface axis `axis3`.
    kernel::CurveSet set;
    int body = -1;
    Vec3 axis3 = Vec3::Zero();
    bool pointLike() const { return kind == Kind::Point || kind == Kind::Circle || kind == Kind::Arc; }
    Point2 point() const { return kind == Kind::Point ? a : center; }  // point or center
};

// Part sketch dimension tolerances (§6а.3, FX3-DRW-001): drawing dimension forms (kind and reference set) — sketch
// dimensions matched to edges that carry a fit designation.
struct SketchFit {
    std::vector<doc::SheetDimension> forms;
    const doc::Json* fit = nullptr;
};
using SketchFits = std::vector<SketchFit>;
SketchFits sketchFits(const doc::Document& md, const doc::Element& el, const regen::PartState& st, const kernel::Kernel& k);
// Cache of sketch tolerances by element ID — for one sheet build.
using FitCache = std::map<std::string, std::shared_ptr<const SketchFits>>;

struct Ctx {
    const ViewMapping& vp;
    const regen::PartState& st;
    const kernel::Kernel& k;
    const doc::Element* annotated = nullptr;  // view model with fit designations (§6а.3)
    const doc::Document* md = nullptr;        // model document (sketch tolerances)
    FitCache* fits = nullptr;
    bool iso = false;        // ISO 129 style (§6а.4)
    bool facePlane = false;  // view is isometric: linear dimension on an edge — in the face plane (§6а.4)
    std::optional<std::pair<Point2, double>> clip;  // detail view: circle K (sheet) — clipping (§6а.5)
    const doc::Sheet* sheet = nullptr;  // the sheet: constructions for references to them (§6.29, FX14-DRW-006)
    std::string viewId;                 // the view of the dimension or construction
    Point2 T(const Point2& q) const { return vp.toSheet(q); }
    const SketchFits* sketchFits() const;
};

Result<RefGeom> edgeGeom(const Ctx& c, int index);
Result<RefGeom> faceLineGeom(const Ctx& c, int faceIndex, bool cut, int side);
// Axis surface of a cylinder or cone face (§6а.2, FX9-DRW-001): axis point A (cone — apex) and direction a, extent [t0, t1]
// along the axis and angular span φ over the faces of the same surface of its body (§6.15 items 1, 2); first — the
// smallest face table index of the group.
struct AxisSurface {
    bool cone = false;
    Point3 A = Point3::Zero();
    Vec3 a = Vec3::UnitZ();
    double radius = 0, halfAngle = 0, t0 = 0, t1 = 0, phi = 0;
    int first = -1;
};
std::optional<AxisSurface> axisSurface(const Ctx& c, int faceIndex);
Result<RefGeom> axisGeom(const Ctx& c, int faceIndex, bool center);
Result<RefGeom> refGeom(const Ctx& c, const doc::DimensionRef& r);
// Curve reference (§6а.2, FX7-DRW-010): its point with the greatest P·D (world direction D) and its polylines in view
// coordinates (deflection DIM_CURVE_DEFLECTION); a kernel refusal — DRAWING_DIM_BAD_REFS (silhouette / curveType).
Result<Point3> curveExtreme(const Ctx& c, const RefGeom& g, const Vec3& D);
Result<std::vector<std::vector<Point3>>> curvePolylines(const Ctx& c, const RefGeom& g);

// View and model of a dimension (§6а.2). viewError — error of the view or model (the dimension gets no error of its own).
struct Setup {
    ViewMapping vp;
    regen::StatePtr state;
};
Result<Setup> setup(const doc::Sheet& sheet, const std::string& viewId, const ModelProvider& models, bool& viewError,
                    const SheetGraphics* built = nullptr);
const regen::PartState& stateOrEmpty(const regen::StatePtr& s);

// Dimension construction (§6а.3–§6а.5); drawing — the drawing element (standard; may be nullptr — GOST).
Result<DimensionPreview> build(const doc::Document& d, const doc::Element* drawing, const doc::Sheet& sheet, const doc::SheetDimension& a,
                               const std::optional<Point2>& cursor, const ModelProvider& models, bool& viewError,
                               const SheetGraphics* built = nullptr, FitCache* fits = nullptr);

// Context of a view built by setup (§6а.2): view model, annotations, standard, detail view circle, the sheet.
Ctx makeCtx(const doc::Document& d, const doc::Element* drawing, const doc::Sheet& sheet, const std::string& viewId, const Setup& su,
            const kernel::Kernel& k, FitCache* fits);

// Construction shape in view coordinates (§6.29, constructions.cpp): geometry (segment — without the axis overshoot, circle,
// arc; depth 0), effective points and the first binding error (its point — by the stored xy); degenerate —
// DRAWING_CONS_DEGENERATE.
struct ConstructionShape {
    RefGeom geom;
    std::vector<Point2> points;
    std::optional<Error> bindError;
};
Result<ConstructionShape> constructionShape(const Ctx& c, const doc::SheetConstruction& cs);
// Dimension reference to a construction (§6.29 «Dimensions»): no such construction — DRAWING_DIM_REF_LOST; another view —
// DRAWING_DIM_BAD_REFS (refs); degenerate — DRAWING_DIM_BAD_REFS (zero).
Result<RefGeom> constructionRefGeom(const Ctx& c, const std::string& id);

bool sameRefs(const doc::SheetDimension& a, const doc::SheetDimension& b);
using geom2::cross2;
// Counterclockwise arc angle from a0 to a1 in (0, 2π].
inline double sweepOf(double a0, double a1) { return geom2::ccwSweepPositive(a0, a1); }

}  // namespace construe::drawing::dimdetail
