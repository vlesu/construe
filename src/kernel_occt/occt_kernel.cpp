// Kernel implementation on OpenCASCADE 8 (spec/kernel-interface.md §9). The only module that includes OCCT headers.
#include "kernel_occt/occt_kernel.h"
#include "kernel_occt/occt_build.h"
#include "kernel_occt/occt_exchange.h"
#include "kernel_occt/occt_hlr.h"

#include "base/ids.h"
#include "base/spline.h"
#include <cstdio>
#include "base/tolerance.h"
#include "base/geom2.h"

#include <BinTools.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <Message_ProgressIndicator.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Domain.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp_Sinert.hxx>
#include <BRepGProp_Vinert.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Pnt2d.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GeomConvert.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <GProp_GProps.hxx>
#include <OSD_Parallel.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <IMeshTools_Parameters.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Precision.hxx>
#include <gp_Lin.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <OSD.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <type_traits>

namespace construe::kernel {

namespace {

// Two faces sharing an edge lie on one surface object (unifyFaces, §3.27): a merge was left undone.
bool sharesSurfaceAcrossEdge(const TopoDS_Shape& s) {
    TopTools_IndexedDataMapOfShapeListOfShape ef;
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, ef);
    for (int i = 1; i <= ef.Extent(); ++i) {
        const auto& fs = ef.FindFromIndex(i);
        if (fs.Extent() != 2) continue;
        const TopoDS_Face& a = TopoDS::Face(fs.First());
        const TopoDS_Face& b = TopoDS::Face(fs.Last());
        if (a.IsSame(b)) continue;  // a seam
        TopLoc_Location la, lb;
        if (BRep_Tool::Surface(a, la) == BRep_Tool::Surface(b, lb) && la.IsEqual(lb)) return true;
    }
    return false;
}


// A cancellation flag as the progress indicator of an OCCT algorithm: the algorithm stops at its next progress check.
class CancelIndicator final : public Message_ProgressIndicator {
public:
    explicit CancelIndicator(const Cancel* cancel) : cancel_(cancel) {}
    Standard_Boolean UserBreak() override { return cancelled(cancel_); }
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {}

private:
    const Cancel* cancel_;
};

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using ShapeList = NCollection_List<TopoDS_Shape>;

gp_Pnt gp(const Point3& p) { return gp_Pnt(p.x(), p.y(), p.z()); }
gp_Dir gd(const Vec3& v) { return gp_Dir(v.x(), v.y(), v.z()); }
gp_Vec gv(const Vec3& v) { return gp_Vec(v.x(), v.y(), v.z()); }
Point3 pt(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
Vec3 vc(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }

// ---------------------------------------------------------------- body data (§9.1)

// Body measurements computed once (§9.9 "Measurement cache"): the body is immutable, so a query result does not depend
// on whether it is the first or a repeated one. Filled under a mutex (queries come from different threads, §1).
struct ShapeMemo {
    std::mutex mu;
    std::vector<std::pair<double, Point3>> faceProps;  // area and center of each face; empty means not computed yet
    std::optional<MassProperties> mass;
    std::optional<ValidityReport> validity;
    std::optional<BBox3> bbox;
};

struct OcctShape final : ShapeData {
    TopoDS_Shape shape;
    std::vector<TopoDS_Shape> solids;
    ShapeMap faces, edges, vertices;
    std::vector<std::vector<int>> edgeFaces;   // with a repeat for the seam
    std::vector<std::vector<int>> vertexFaces, vertexEdges;
    std::vector<std::pair<int, int>> edgeVerts;
    std::unique_ptr<ShapeMemo> memo = std::make_unique<ShapeMemo>();
};

std::shared_ptr<OcctShape> makeShapeData(const std::vector<TopoDS_Shape>& solids) {
    auto d = std::make_shared<OcctShape>();
    TopoDS_Compound comp;
    BRep_Builder b;
    b.MakeCompound(comp);
    for (const auto& s : solids) b.Add(comp, s);
    d->shape = comp;
    d->solids = solids;
    for (TopExp_Explorer ex(comp, TopAbs_FACE); ex.More(); ex.Next()) d->faces.Add(ex.Current());
    for (TopExp_Explorer ex(comp, TopAbs_EDGE); ex.More(); ex.Next())
        if (!BRep_Tool::Degenerated(TopoDS::Edge(ex.Current()))) d->edges.Add(ex.Current());
    for (TopExp_Explorer ex(comp, TopAbs_VERTEX); ex.More(); ex.Next()) d->vertices.Add(ex.Current());
    d->edgeFaces.assign(d->edges.Extent(), {});
    d->vertexFaces.assign(d->vertices.Extent(), {});
    d->vertexEdges.assign(d->vertices.Extent(), {});
    d->edgeVerts.assign(d->edges.Extent(), {0, 0});
    for (int f = 1; f <= d->faces.Extent(); ++f) {
        for (TopExp_Explorer ex(d->faces(f), TopAbs_EDGE); ex.More(); ex.Next()) {
            const int e = d->edges.FindIndex(ex.Current());
            if (e > 0) d->edgeFaces[e - 1].push_back(f - 1);
        }
        std::set<int> vs;
        for (TopExp_Explorer ex(d->faces(f), TopAbs_VERTEX); ex.More(); ex.Next()) vs.insert(d->vertices.FindIndex(ex.Current()));
        for (int v : vs)
            if (v > 0) d->vertexFaces[v - 1].push_back(f - 1);
    }
    for (int e = 1; e <= d->edges.Extent(); ++e) {
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(TopoDS::Edge(d->edges(e)), v1, v2);
        const int i1 = d->vertices.FindIndex(v1) - 1, i2 = d->vertices.FindIndex(v2) - 1;
        d->edgeVerts[e - 1] = {std::min(i1, i2), std::max(i1, i2)};
        for (int v : {i1, i2}) {
            auto& lst = d->vertexEdges[v];
            if (std::find(lst.begin(), lst.end(), e - 1) == lst.end()) lst.push_back(e - 1);
        }
    }
    for (auto& l : d->vertexFaces) std::sort(l.begin(), l.end());
    for (auto& l : d->vertexEdges) std::sort(l.begin(), l.end());
    return d;
}

std::vector<TopoDS_Shape> solidsOf(const TopoDS_Shape& s) {
    std::vector<TopoDS_Shape> out;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) out.push_back(ex.Current());
    return out;
}

const OcctShape& D(const Shape& s) {
    // A null or foreign shape is a contract violation (§3.0а): a standard exception instead of a null dereference.
    const auto* d = dynamic_cast<const OcctShape*>(s.data());
    if (!d) throw std::invalid_argument(s.null() ? "kernel: null shape" : "kernel: shape of another kernel");
    return *d;
}

Shape wrap(std::shared_ptr<OcctShape> d) { return Shape(std::move(d)); }

Error kerr(const char* code) { return Error(code); }

// Sorting an Origin list (§2.6) and removing duplicates.
int originRank(const Origin& o) { return static_cast<int>(o.kind); }
void sortOrigins(std::vector<Origin>& v) {
    std::stable_sort(v.begin(), v.end(), [](const Origin& a, const Origin& b) {
        if (originRank(a) != originRank(b)) return originRank(a) < originRank(b);
        const int oa = a.kind == Origin::Kind::FromFace ? a.input.operand : a.operand;
        const int ob = b.kind == Origin::Kind::FromFace ? b.input.operand : b.operand;
        if (oa != ob) return oa < ob;
        if (a.kind == Origin::Kind::FromFace) return a.input.face < b.input.face;
        if (a.kind == Origin::Kind::FromEdge) return a.edges < b.edges;
        if (a.kind == Origin::Kind::FromVertex) return a.vertex < b.vertex;
        return compareIds(a.segment.empty() ? "e0" : a.segment, b.segment.empty() ? "e0" : b.segment) < 0;
    });
    std::vector<Origin> out;
    for (const auto& o : v) {
        bool dup = false;
        for (const auto& x : out)
            dup = dup || (x.kind == o.kind && x.role == o.role && x.segment == o.segment && x.input == o.input &&
                          x.modified == o.modified && x.operand == o.operand && x.edges == o.edges && x.vertex == o.vertex);
        if (!dup) out.push_back(o);
    }
    v = std::move(out);
}

// ---------------------------------------------------------------- profile → face (§9.3)

// Spline segment (§9.3, S4A-SPL-003): Geom_BSplineCurve with the same poles, weights, knots, multiplicities and degree.
Handle(Geom_BSplineCurve) splineCurve3(const Segment3& s) {
    const int n = static_cast<int>(s.poles.size());
    TColgp_Array1OfPnt P(1, n);
    TColStd_Array1OfReal W(1, n);
    for (int i = 0; i < n; ++i) {
        P.SetValue(i + 1, gp(s.poles[static_cast<size_t>(i)]));
        W.SetValue(i + 1, s.weights.empty() ? 1.0 : s.weights[static_cast<size_t>(i)]);
    }
    const int k = static_cast<int>(s.knots.size());
    TColStd_Array1OfReal K(1, k);
    TColStd_Array1OfInteger M(1, k);
    for (int i = 0; i < k; ++i) {
        K.SetValue(i + 1, s.knots[static_cast<size_t>(i)]);
        M.SetValue(i + 1, s.mults[static_cast<size_t>(i)]);
    }
    return new Geom_BSplineCurve(P, W, K, M, s.degree);
}

// Points of a spline segment for checks against an axis (§3.3): its flattening (§2.7.6) with the profile gap tolerance.
std::vector<Point3> splineSamples(const Segment3& s, const Plane& pl) {
    spline::Curve c;
    c.degree = s.degree;
    for (const auto& q : s.poles) c.poles.push_back(pl.toLocal(q));
    c.weights = s.weights;
    c.knots = s.knots;
    c.mults = s.mults;
    std::vector<Point3> out;
    if (!spline::validate(c).empty()) return out;
    const spline::Flat f = spline::expand(c);
    for (double t : spline::flattenParams(f, KERNEL_SPLINE_CHECK_TOL)) out.push_back(pl.toWorld(spline::eval(f, t)));
    return out;
}

struct BuiltFace {
    TopoDS_Face face;
    std::vector<std::pair<TopoDS_Shape, SegmentId>> edgeIds;
};

Result<BuiltFace> buildFace(const Profile& p) {
    BuiltFace bf;
    const gp_Ax3 ax(gp(p.plane.origin), gd(p.plane.normal), gd(p.plane.x));
    auto makeWire = [&](const Loop3& loop) -> Result<TopoDS_Wire> {
        BRepBuilderAPI_MakeWire mw;
        const size_t n = loop.segments.size();
        if (n == 1 && loop.segments[0].kind == Segment3::Kind::Ellipse) {
            // Ellipse (§2.5, S4-SKT-004): gp_Elips wants the major semi-axis first; radius < minorRadius swaps the axes.
            const auto& s = loop.segments[0];
            const bool swap = s.radius < s.minorRadius;
            const Vec3 major = swap ? Vec3(p.plane.normal.cross(s.xDir)) : s.xDir;
            gp_Elips el(gp_Ax2(gp(s.center), gd(p.plane.normal), gd(major)), std::max(s.radius, s.minorRadius),
                        std::min(s.radius, s.minorRadius));
            BRepBuilderAPI_MakeEdge me(el);
            if (!me.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
            TopoDS_Edge e = me.Edge();
            if (s.reversed) e.Reverse();
            bf.edgeIds.push_back({e, s.id});
            mw.Add(e);
            return mw.Wire();
        }
        if (n == 1 && loop.segments[0].kind == Segment3::Kind::Circle) {
            const auto& s = loop.segments[0];
            gp_Circ c(gp_Ax2(gp(s.center), gd(p.plane.normal), gd(p.plane.x)), s.radius);
            TopoDS_Edge e = BRepBuilderAPI_MakeEdge(c).Edge();
            if (s.reversed) e.Reverse();
            bf.edgeIds.push_back({e, s.id});
            mw.Add(e);
            return mw.Wire();
        }
        auto from = [&](const Segment3& s) {
            const Point3& a = s.kind == Segment3::Kind::Line ? s.p1 : s.kind == Segment3::Kind::Spline ? s.poles.front() : s.start;
            const Point3& b = s.kind == Segment3::Kind::Line ? s.p2 : s.kind == Segment3::Kind::Spline ? s.poles.back() : s.end;
            return s.reversed ? b : a;
        };
        auto to = [&](const Segment3& s) {
            const Point3& a = s.kind == Segment3::Kind::Line ? s.p1 : s.kind == Segment3::Kind::Spline ? s.poles.front() : s.start;
            const Point3& b = s.kind == Segment3::Kind::Line ? s.p2 : s.kind == Segment3::Kind::Spline ? s.poles.back() : s.end;
            return s.reversed ? a : b;
        };
        // Knots (§2.5): between end i and start i+1.
        std::vector<TopoDS_Vertex> nodes(n);
        for (size_t i = 0; i < n; ++i) {
            const Segment3& a = loop.segments[i];
            const Segment3& b = loop.segments[(i + 1) % n];
            // Curve ends are exact (arcs and ellipse arcs, S4-SKT-007).
            const bool aArc = a.kind == Segment3::Kind::Arc || a.kind == Segment3::Kind::EllipseArc || a.kind == Segment3::Kind::Spline;
            const bool bArc = b.kind == Segment3::Kind::Arc || b.kind == Segment3::Kind::EllipseArc || b.kind == Segment3::Kind::Spline;
            Point3 q;
            if (aArc && bArc) q = (to(a) + from(b)) / 2;
            else if (aArc) q = to(a);
            else if (bArc) q = from(b);
            else q = (to(a) + from(b)) / 2;
            nodes[i] = BRepBuilderAPI_MakeVertex(gp(q)).Vertex();
        }
        for (size_t i = 0; i < n; ++i) {
            const Segment3& s = loop.segments[i];
            const TopoDS_Vertex& vFrom = nodes[(i + n - 1) % n];
            const TopoDS_Vertex& vTo = nodes[i];
            TopoDS_Edge e;
            if (s.kind == Segment3::Kind::Line) {
                BRepBuilderAPI_MakeEdge me(vFrom, vTo);
                if (!me.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
                e = me.Edge();
                bf.edgeIds.push_back({e, s.id});
            } else if (s.kind == Segment3::Kind::Spline) {
                const TopoDS_Vertex& vs = s.reversed ? vTo : vFrom;  // natural start
                const TopoDS_Vertex& ve = s.reversed ? vFrom : vTo;
                BRepBuilderAPI_MakeEdge me(splineCurve3(s), vs, ve);
                if (!me.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
                e = me.Edge();
                bf.edgeIds.push_back({e, s.id});
                if (s.reversed) e.Reverse();
            } else if (s.kind == Segment3::Kind::EllipseArc) {
                // Ellipse arc (§9.3, S4-SKT-007): the major semi-axis first (like the ellipse), counterclockwise.
                const bool swap = s.radius < s.minorRadius;
                const Vec3 major = swap ? Vec3(p.plane.normal.cross(s.xDir)) : s.xDir;
                gp_Elips el(gp_Ax2(gp(s.center), gd(p.plane.normal), gd(major)), std::max(s.radius, s.minorRadius),
                            std::min(s.radius, s.minorRadius));
                const TopoDS_Vertex& vs = s.reversed ? vTo : vFrom;  // natural start
                const TopoDS_Vertex& ve = s.reversed ? vFrom : vTo;
                BRepBuilderAPI_MakeEdge me(el, vs, ve);
                if (!me.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
                e = me.Edge();
                bf.edgeIds.push_back({e, s.id});
                if (s.reversed) e.Reverse();
            } else {
                gp_Circ c(gp_Ax2(gp(s.center), gd(p.plane.normal), gd(p.plane.x)), s.radius);
                const TopoDS_Vertex& vs = s.reversed ? vTo : vFrom;  // natural start
                const TopoDS_Vertex& ve = s.reversed ? vFrom : vTo;
                BRepBuilderAPI_MakeEdge me(c, vs, ve);
                if (!me.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
                e = me.Edge();
                bf.edgeIds.push_back({e, s.id});
                if (s.reversed) e.Reverse();
            }
            mw.Add(e);
        }
        if (!mw.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
        return mw.Wire();
    };
    auto outer = makeWire(p.outer);
    if (!outer) return outer.error();
    BRepBuilderAPI_MakeFace mf(gp_Pln(ax), *outer, true);
    for (const auto& h : p.holes) {
        auto w = makeWire(h);
        if (!w) return w.error();
        mf.Add(*w);
    }
    if (!mf.IsDone()) return kerr("KERNEL_OPERATION_FAILED");
    bf.face = mf.Face();
    if (!BRepCheck_Analyzer(bf.face).IsValid()) return kerr("KERNEL_OPERATION_FAILED");
    return bf;
}

// ---------------------------------------------------------------- profile face origins (§2.6, §9.10)

// Side faces generated from the profile edges get Profile/Side with the segment ID (by the builder's Generated history).
void tagProfileSides(BRepBuilderAPI_MakeShape& b, const OcctShape& sd, const std::vector<std::pair<TopoDS_Shape, SegmentId>>& edgeIds,
                     History& h) {
    for (const auto& [e, id] : edgeIds)
        for (const TopoDS_Shape& g : b.Generated(e)) {
            if (g.ShapeType() != TopAbs_FACE) continue;
            const int fi = sd.faces.FindIndex(g);
            if (fi <= 0) continue;
            Origin o;
            o.kind = Origin::Kind::Profile;
            o.role = ProfileRole::Side;
            o.segment = id;
            h.faceOrigins[fi - 1].push_back(o);
        }
}

// A cap face (start/end of the sweep) gets Profile/role; a null shape or a face not in the result is skipped.
void tagProfileCap(const OcctShape& sd, const TopoDS_Shape& sh, ProfileRole role, History& h) {
    const int fi = sh.IsNull() ? 0 : sd.faces.FindIndex(sh);
    if (fi <= 0) return;
    Origin o;
    o.kind = Origin::Kind::Profile;
    o.role = role;
    h.faceOrigins[fi - 1].push_back(o);
}

// Sides and both caps by the builder's history (extrude, loft): the common part of profile operations.
template <class Builder>
void tagProfileFaces(Builder& b, const OcctShape& sd, const std::vector<std::pair<TopoDS_Shape, SegmentId>>& edgeIds, History& h) {
    tagProfileSides(b, sd, edgeIds, h);
    tagProfileCap(sd, b.FirstShape(), ProfileRole::CapStart, h);
    tagProfileCap(sd, b.LastShape(), ProfileRole::CapEnd, h);
}

// A face without Generated history: Profile/Side of every profile segment whose edge midpoint lies on the face (§9.10).
void tagSideByMidpoint(const TopoDS_Face& face, const std::vector<std::pair<TopoDS_Shape, SegmentId>>& edgeIds, std::vector<Origin>& out) {
    for (const auto& [e, id] : edgeIds) {
        BRepAdaptor_Curve c(TopoDS::Edge(e));
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(mid).Vertex(), face);
        if (dist.IsDone() && dist.Value() <= KERNEL_PROFILE_GAP_TOL) {
            Origin x;
            x.role = ProfileRole::Side;
            x.segment = id;
            out.push_back(x);
        }
    }
}

// ---------------------------------------------------------------- properties (§9.9)

// Face count threshold from which per-face integration runs in parallel (§9.2).
constexpr int KERNEL_PARALLEL_MIN_FACES = 8;

// Rough center, as in BRepGProp: mean of vertex points in TopExp_Explorer traversal order (with repeats).
gp_Pnt roughBaryCenter(const TopoDS_Shape& s) {
    gp_XYZ xyz(0, 0, 0);
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next(), ++n) xyz += BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())).XYZ();
    if (n > 0) xyz /= n;
    return gp_Pnt(xyz);
}

// Face integrals with tolerance Eps = 1e-10, each face independently and in parallel; summed in items order.
// Reproduces BRepGProp::VolumeProperties(S, P, 1e-10, OnlyClosed = true) and BRepGProp::SurfaceProperties(S, P, 1e-10)
// (OCCT 8.0.1) bit for bit: same integrators, same reference points, same GProp_GProps::Add order (§9.9).
struct FaceItem {
    TopoDS_Face face;
    gp_Pnt loc;
};
template <class Inert>
std::optional<GProp_GProps> integrateFaces(const std::vector<FaceItem>& items, const gp_Pnt& origin, double eps = 1e-10) {
    // OCCT handles a face without wires (natural bounds) or without a surface differently; then no parallelism.
    for (const auto& it : items) {
        TopLoc_Location l;
        if (it.face.NbChildren() == 0 || BRep_Tool::Surface(it.face, l).IsNull()) return std::nullopt;
    }
    std::vector<GProp_GProps> parts(items.size());
    OSD_Parallel::For(0, int(items.size()), [&](int i) {
        BRepGProp_Face bf;
        BRepGProp_Domain bd;
        bf.Load(items[size_t(i)].face);
        bd.Init(items[size_t(i)].face);
        Inert g;
        g.SetLocation(items[size_t(i)].loc);
        if (eps < 1) g.Perform(bf, bd, eps);
        else g.Perform(bf, bd);  // non-adaptive Gauss quadrature (as OCCT does for Eps ≥ 1)
        parts[size_t(i)] = g;
    }, int(items.size()) < KERNEL_PARALLEL_MIN_FACES);
    GProp_GProps out(origin);
    for (const auto& p : parts) out.Add(p);
    return out;
}

gp_Pnt propsOrigin(const TopoDS_Shape& s) {
    gp_Pnt p(0, 0, 0);
    p.Transform(s.Location());
    return p;
}

// Volume properties of the closed shells of the shape (as VolumeProperties(s, 1e-10, OnlyClosed = true)); eps ≥ 1 means
// non-adaptive (as VolumeProperties(s, OnlyClosed = true)).
GProp_GProps volumeProps(const TopoDS_Shape& s, double eps = 1e-10) {
    std::vector<FaceItem> items;
    for (TopExp_Explorer sh(s, TopAbs_SHELL); sh.More(); sh.Next()) {
        if (!BRep_Tool::IsClosed(sh.Current())) continue;
        const gp_Pnt loc = roughBaryCenter(sh.Current());
        for (TopExp_Explorer f(sh.Current(), TopAbs_FACE); f.More(); f.Next()) {
            const auto o = f.Current().Orientation();
            if (o == TopAbs_FORWARD || o == TopAbs_REVERSED) items.push_back({TopoDS::Face(f.Current()), loc});
        }
    }
    if (auto g = integrateFaces<BRepGProp_Vinert>(items, propsOrigin(s), eps)) return *g;
    GProp_GProps g;
    if (eps < 1) BRepGProp::VolumeProperties(s, g, eps, true);
    else BRepGProp::VolumeProperties(s, g, true);
    return g;
}

// Surface properties of the shape (as SurfaceProperties(s, 1e-10)).
GProp_GProps surfaceProps(const TopoDS_Shape& s) {
    std::vector<FaceItem> items;
    const gp_Pnt loc = roughBaryCenter(s);
    for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next()) items.push_back({TopoDS::Face(f.Current()), loc});
    if (auto g = integrateFaces<BRepGProp_Sinert>(items, propsOrigin(s))) return *g;
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g, 1e-10);
    return g;
}

// Areas and centers of all body faces (measurement cache, §9.9): via SurfaceProperties(face, 1e-10), faces in parallel.
const std::vector<std::pair<double, Point3>>& faceProps(const OcctShape& d) {
    std::lock_guard lk(d.memo->mu);
    auto& fp = d.memo->faceProps;
    const int n = d.faces.Extent();
    if (n == 0 || !fp.empty()) return fp;
    std::vector<std::pair<double, Point3>> out(static_cast<size_t>(n));
    OSD_Parallel::For(0, n, [&](int i) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(d.faces(i + 1), g, 1e-10);
        out[size_t(i)] = {g.Mass(), pt(g.CentreOfMass())};
    }, n < KERNEL_PARALLEL_MIN_FACES);
    fp = std::move(out);
    return fp;
}

double maxTolerance(const TopoDS_Shape& s) {
    double t = 0;
    for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next()) t = std::max(t, BRep_Tool::Tolerance(TopoDS::Vertex(ex.Current())));
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) t = std::max(t, BRep_Tool::Tolerance(TopoDS::Edge(ex.Current())));
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) t = std::max(t, BRep_Tool::Tolerance(TopoDS::Face(ex.Current())));
    return t;
}

class OcctKernel final : public Kernel {
public:
    OcctKernel() { occt::ensureSharedStatics(); }  // OCCT signal handlers are not installed (Р-015)

    // ------------------------------------------------------------ result validation
    // ---------------------------------------------------------------- view projection (§3.17, §9.9б)

    Result<HlrProjection> hiddenLineProjection(const std::vector<PlacedShape>& items, const ViewDirection& view, const HlrParams& params,
                                               const Cancel* cancel) const override {
        auto occ = hlrInputs(items, view, params);
        if (!occ) return occ.error();
        return occt::hlrProject(*occ, view, params, cancel);
    }

    // Validation of projection/sectioning inputs (§3.17 "Inputs and errors") → OCCT bodies, placements (no solid: skip).
    static Result<std::vector<std::pair<TopoDS_Shape, gp_Trsf>>> hlrInputs(const std::vector<PlacedShape>& items, const ViewDirection& view,
                                                                         const HlrParams& params) {
        auto finite = [](const Vec3& v) { return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()); };
        const Vec3 &n = view.toViewer, &u = view.up;
        if (!finite(n) || !finite(u) || std::abs(n.norm() - 1) > 1e-9 || std::abs(u.norm() - 1) > 1e-9 || std::abs(n.dot(u)) > 1e-9)
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("view"));
        if (!std::isfinite(params.chordTol) || params.chordTol < KERNEL_MIN_CHORD || params.chordTol > KERNEL_MAX_CHORD)
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol"));
        std::vector<std::pair<TopoDS_Shape, gp_Trsf>> occ;
        for (const auto& it : items) {
            const auto& r = it.motion.r;
            Eigen::Matrix3d R;
            R << r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8];
            if (!R.allFinite() || !finite(it.motion.t) || ((R.transpose() * R) - Eigen::Matrix3d::Identity()).cwiseAbs().maxCoeff() > 1e-9 ||
                R.determinant() < 0)
                return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("motion"));
            if (it.shape.null() || D(it.shape).solids.empty()) continue;
            gp_Trsf t;
            t.SetValues(r[0], r[1], r[2], it.motion.t.x(), r[3], r[4], r[5], it.motion.t.y(), r[6], r[7], r[8], it.motion.t.z());
            occ.push_back({D(it.shape).shape, t});
        }
        return occ;
    }

    // ---------------------------------------------------------------- sectioning for section views (§3.18, §9.9в)

    Result<SectionCut> sectionCut(const std::vector<PlacedShape>& items, const ViewDirection& view, const Point3& origin,
                                  const HlrParams& params, const Cancel* cancel) const override {
        if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) || !std::isfinite(origin.z()))
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("origin"));
        auto occ = hlrInputs(items, view, params);
        if (!occ) return occ.error();
        std::vector<int> index;  // index of the items element for each occ input (bodies without a solid are skipped)
        for (size_t i = 0; i < items.size(); ++i)
            if (!items[i].shape.null() && !D(items[i].shape).solids.empty()) index.push_back(int(i));
        auto r = occt::sectionCutOcct(*occ, view, origin, params.chordTol, cancel);
        if (!r) return r.error();
        SectionCut out;
        for (auto& [i, shape] : r->bodies) out.bodies.push_back({wrap(makeShapeData(solidsOf(shape))), {}});
        for (auto& reg : r->regions) {
            reg.item = index[size_t(reg.item)];
            out.regions.push_back(std::move(reg));
        }
        return out;
    }

    ValidityReport checkValidity(const Shape& s) const override {
        const OcctShape& d = D(s);
        std::lock_guard lk(d.memo->mu);
        if (d.memo->validity) return *d.memo->validity;
        ValidityReport r;
        try {
            // Parallel check of subshapes (§9.2): the same set of checks, a different traversal order.
            if (!d.solids.empty() && !BRepCheck_Analyzer(d.shape, true, /*theIsParallel*/ true).IsValid()) r.issues.push_back("brepcheck");
        } catch (const Standard_Failure&) {
            r.issues.push_back("exception");
        }
        for (size_t e = 0; e < d.edgeFaces.size(); ++e)
            if (d.edgeFaces[e].size() != 2) r.issues.push_back("edge-not-manifold");
        for (const auto& sol : d.solids) {
            // Only the sign of the volume is needed (orientation, §3.10): non-adaptive quadrature (§9.9).
            if (!(volumeProps(sol, 1.0).Mass() > 0)) r.issues.push_back("solid-volume");
        }
        if (maxTolerance(d.shape) > KERNEL_MAX_ENTITY_TOL) r.issues.push_back("tolerance");
        r.valid = r.issues.empty();
        d.memo->validity = r;
        return r;
    }

    // ------------------------------------------------------------ shape healing (§3.24, §9.6з; S4-REC-013)
    Result<HealResult> healShape(const Shape& s, double maxTol) const override {
        if (s.null()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("shape"));
        const OcctShape& d = D(s);
        HealResult r;
        r.shape = s;
        r.toleranceBefore = r.toleranceAfter = maxTolerance(d.shape);
        if (r.toleranceBefore <= KERNEL_MAX_ENTITY_TOL || !(maxTol > 0) || r.toleranceBefore > maxTol) return r;
        auto healed = occt::healShape(d.shape, maxTol);
        if (!healed) return r;
        auto nd = makeShapeData(solidsOf(*healed));
        if (nd->solids.size() != d.solids.size()) return r;
        Shape out = wrap(nd);
        if (!checkValidity(out).valid) return r;
        // The shape is almost the same (§3.24): the volume changes by no more than area·input tolerance.
        const auto m0 = massProperties(s), m1 = massProperties(out);
        if (std::abs(m1.volume - m0.volume) > m0.area * r.toleranceBefore + KERNEL_MASSPROP_REL * m0.volume) return r;
        r.shape = out;
        r.healed = true;
        r.toleranceAfter = maxTolerance(nd->shape);
        return r;
    }

    // ------------------------------------------------------------ extrusion (§9.4)
    Result<OpResult> extrude(const Profile& pIn, const Extent& ext, bool reverse) const override {
        Profile p;
        auto info = validateProfile(pIn, &p);
        if (!info) return info.error();
        double depth = ext.depth;
        Extent::Kind kind = ext.kind;
        double back = 0;  // ThroughAllBoth: shift the base backwards (§9.4)
        if (kind == Extent::Kind::ThroughAllBoth) {
            if (ext.bounds.null()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("bounds"));
            const double fwd = throughAllDepth(*this, ext.bounds, p.plane.origin, p.plane.normal);
            back = throughAllDepth(*this, ext.bounds, p.plane.origin, p.plane.normal * -1.0);
            depth = fwd + back;
        }
        if (kind == Extent::Kind::ThroughAll) {
            // §4.5: depth from bbox(bounds), then as Blind.
            if (ext.bounds.null()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("bounds"));
            depth = throughAllDepth(*this, ext.bounds, p.plane.origin, p.plane.normal * (reverse ? -1.0 : 1.0));
            kind = Extent::Kind::Blind;
        }
        if (!std::isfinite(depth) || depth < KERNEL_MIN_DIMENSION || depth > KERNEL_MAX_COORD) {
            Error e("KERNEL_INVALID_DIMENSION");
            e.with("name", std::string("depth")).with("actual", depth);
            e.with("expected", depth < KERNEL_MIN_DIMENSION ? KERNEL_MIN_DIMENSION : KERNEL_MAX_COORD);
            return e;
        }
        try {
            auto bf = buildFace(p);
            if (!bf) return bf.error().code == "KERNEL_OPERATION_FAILED" ? Result<OpResult>(Error("KERNEL_OPERATION_FAILED").with("name", std::string("extrude"))) : Result<OpResult>(bf.error());
            Vec3 d = p.plane.normal * (reverse && kind == Extent::Kind::Blind ? -1.0 : 1.0);
            TopoDS_Shape base = bf->face;
            std::vector<std::pair<TopoDS_Shape, SegmentId>> edgeIds = bf->edgeIds;
            if (kind == Extent::Kind::Symmetric || kind == Extent::Kind::ThroughAllBoth) {
                gp_Trsf t;
                t.SetTranslation(gv(-p.plane.normal * (kind == Extent::Kind::Symmetric ? depth / 2 : back)));
                BRepBuilderAPI_Transform tr(bf->face, t, false);
                base = tr.Shape();
                for (auto& [e, id] : edgeIds) e = tr.ModifiedShape(e);
                d = p.plane.normal;
            }
            BRepPrimAPI_MakePrism prism(base, gv(d * depth), false, true);
            if (!prism.IsDone()) return Error("KERNEL_OPERATION_FAILED").with("name", std::string("extrude"));
            auto sd = makeShapeData(solidsOf(prism.Shape()));
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            tagProfileFaces(prism, *sd, edgeIds, r.history);
            for (auto& o : r.history.faceOrigins) {
                if (o.empty()) return Error("KERNEL_OPERATION_FAILED").with("name", std::string("extrude"));
                sortOrigins(o);
            }
            r.shape = wrap(sd);
            if (!checkValidity(r.shape).valid) return Error("KERNEL_RESULT_INVALID").with("name", std::string("extrude"));
            return r;
        } catch (const Standard_Failure& f) {
            return Error("KERNEL_INTERNAL").with("detail", std::string(f.what() ? f.what() : "Standard_Failure"));
        } catch (...) {
            return Error("KERNEL_INTERNAL").with("detail", std::string("unknown"));
        }
    }

    // ------------------------------------------------------------ revolution (§9.5)
    Result<OpResult> revolve(const Profile& pIn, const Axis& axis, const RevolveExtent& ext) const override {
        Profile p;
        auto info = validateProfile(pIn, &p);
        if (!info) return info.error();
        auto finite = [](const Vec3& v) { return v.allFinite() && v.cwiseAbs().maxCoeff() <= KERNEL_MAX_COORD; };
        if (!finite(axis.origin) || !finite(axis.direction) || std::abs(axis.direction.norm() - 1) > 1e-9)
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("axis"));
        double angle = 2 * PI;
        bool full = ext.kind == RevolveExtent::Kind::Full;
        if (!full) {
            if (!(ext.value >= KERNEL_MIN_ANGLE && ext.value <= 2 * PI + KERNEL_FULL_TURN_TOL)) {
                Error e("KERNEL_INVALID_DIMENSION");
                e.with("name", std::string("angle")).with("actual", ext.value);
                e.with("expected", ext.value < KERNEL_MIN_ANGLE ? KERNEL_MIN_ANGLE : 2 * PI);
                return e;
            }
            angle = ext.value;
            if (angle >= 2 * PI - KERNEL_FULL_TURN_TOL) full = true;
        }
        const Vec3& n = p.plane.normal;
        const double dz = std::abs(axis.direction.dot(n));
        const double dist = std::abs((axis.origin - p.plane.origin).dot(n));
        if (dz > KERNEL_AXIS_ANGLE_TOL || dist > KERNEL_PROFILE_GAP_TOL) {
            Error e("KERNEL_AXIS_NOT_IN_PLANE");
            e.with("angle", std::asin(std::min(1.0, dz))).with("distance", dist);
            return e;
        }
        // Profile on one side of the axis (§3.3 items 5–6).
        const Vec3 nAxis = n.cross(axis.direction).normalized();
        auto sdist = [&](const Point3& q) { return (q - axis.origin).dot(nAxis); };
        int side = 0;
        auto checkPoint = [&](const Point3& q, const SegmentId& id) -> std::optional<Error> {
            const double s = sdist(q);
            if (std::abs(s) <= KERNEL_PROFILE_GAP_TOL) return std::nullopt;
            const int sg = s > 0 ? 1 : -1;
            if (side == 0) side = sg;
            else if (sg != side) return Error("KERNEL_PROFILE_CROSSES_AXIS").with("id", id);
            return std::nullopt;
        };
        std::vector<const Loop3*> loops{&p.outer};
        for (const auto& h : p.holes) loops.push_back(&h);
        std::optional<Error> touches;
        for (const Loop3* l : loops)
            for (const auto& s : l->segments) {
                std::vector<Point3> pts;
                if (s.kind == Segment3::Kind::Line) pts = {s.p1, s.p2};
                else if (s.kind == Segment3::Kind::Spline) {
                    // Spline (S4A-SPL-003): its samples; an interior sample on the axis — touching.
                    const auto smp = splineSamples(s, p.plane);
                    for (size_t i = 0; i < smp.size(); ++i) {
                        const bool end = i == 0 || i + 1 == smp.size();
                        if (!end && std::abs(sdist(smp[i])) <= KERNEL_PROFILE_GAP_TOL) {
                            if (!touches) touches = Error("KERNEL_PROFILE_TOUCHES_AXIS").with("id", s.id);
                        } else {
                            pts.push_back(smp[i]);
                        }
                    }
                } else if (s.kind == Segment3::Kind::Ellipse || s.kind == Segment3::Kind::EllipseArc) {
                    // Ellipse extremes along the axis normal (§4.3, S4-SKT-004): o ± (a²(n·û)û + b²(n·v̂)v̂)/h.
                    const Vec3 u = s.xDir, v = n.cross(s.xDir);
                    const double nu = nAxis.dot(u), nv = nAxis.dot(v);
                    const double h = std::hypot(s.radius * nu, s.minorRadius * nv);
                    // Ellipse arc (S4-SKT-007): its ends and the extremes lying on the arc.
                    const bool arcOf = s.kind == Segment3::Kind::EllipseArc;
                    auto param = [&](const Point3& q) {
                        const Vec3 w = q - s.center;
                        return std::atan2(w.dot(v) / s.minorRadius, w.dot(u) / s.radius);
                    };
                    if (arcOf) pts = {s.start, s.end};
                    for (double sg : {1.0, -1.0}) {
                        const Point3 x = h > 0 ? Point3(s.center + sg * (s.radius * s.radius * nu * u + s.minorRadius * s.minorRadius * nv * v) / h)
                                               : s.center;
                        if (arcOf) {
                            const double t0 = param(s.start);
                            const double tx = geom2::ccwSweep(t0, param(x)), te = geom2::ccwSweepPositive(t0, param(s.end));
                            if (!(tx > 1e-12 && tx < te - 1e-12)) continue;
                        }
                        if (std::abs(sdist(x)) <= KERNEL_PROFILE_GAP_TOL) {
                            if (!touches) touches = Error("KERNEL_PROFILE_TOUCHES_AXIS").with("id", s.id);
                        } else {
                            pts.push_back(x);
                        }
                    }
                } else {
                    if (s.kind == Segment3::Kind::Arc) pts = {s.start, s.end};
                    for (double sg : {1.0, -1.0}) {
                        const Point3 x = s.center + sg * s.radius * nAxis;
                        bool onArc = s.kind == Segment3::Kind::Circle;
                        if (!onArc) {
                            const Vec3 a = s.start - s.center, xb = x - s.center;
                            double phi = std::atan2(a.cross(xb).dot(n), a.dot(xb));
                            if (phi < 0) phi += 2 * PI;
                            const Vec3 e2 = s.end - s.center;
                            double alpha = std::atan2(a.cross(e2).dot(n), a.dot(e2));
                            if (alpha <= 0) alpha += 2 * PI;
                            onArc = phi > 1e-12 && phi < alpha - 1e-12;
                        }
                        if (onArc) {
                            // Touching (item 6) is checked after intersection (item 5) over all segments.
                            if (std::abs(sdist(x)) <= KERNEL_PROFILE_GAP_TOL) {
                                if (!touches) touches = Error("KERNEL_PROFILE_TOUCHES_AXIS").with("id", s.id);
                            } else {
                                pts.push_back(x);
                            }
                        }
                    }
                }
                for (const Point3& q : pts)
                    if (auto e = checkPoint(q, s.id)) return *e;
            }
        if (touches) return *touches;
        try {
            auto bf = buildFace(p);
            if (!bf) return Error("KERNEL_OPERATION_FAILED").with("name", std::string("revolve"));
            TopoDS_Shape base = bf->face;
            auto edgeIds = bf->edgeIds;
            const gp_Ax1 ax(gp(axis.origin), gd(axis.direction));
            if (!full && ext.kind == RevolveExtent::Kind::Symmetric) {
                gp_Trsf t;
                t.SetRotation(ax, -angle / 2);
                BRepBuilderAPI_Transform tr(bf->face, t, false);
                base = tr.Shape();
                for (auto& [e, id] : edgeIds) e = tr.ModifiedShape(e);
            }
            // Full revolution uses the same constructor with angle 2π: the Generated history for the profile edges
            // is complete (the constructor without an angle does not provide it for some side faces in OCCT 8.0).
            auto rev = std::make_unique<BRepPrimAPI_MakeRevol>(base, ax, full ? 2 * PI : angle, false);
            if (!rev->IsDone()) return Error("KERNEL_OPERATION_FAILED").with("name", std::string("revolve"));
            auto sd = makeShapeData(solidsOf(rev->Shape()));
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            if (full) tagProfileSides(*rev, *sd, edgeIds, r.history);
            else tagProfileFaces(*rev, *sd, edgeIds, r.history);
            // Faces without Generated: the profile segment whose edge midpoint lies on the face (§9.10).
            for (int fi = 1; fi <= sd->faces.Extent(); ++fi)
                if (r.history.faceOrigins[fi - 1].empty())
                    tagSideByMidpoint(TopoDS::Face(sd->faces(fi)), edgeIds, r.history.faceOrigins[fi - 1]);
            for (auto& o : r.history.faceOrigins) {
                if (o.empty()) return Error("KERNEL_OPERATION_FAILED").with("name", std::string("revolve"));
                sortOrigins(o);
            }
            r.shape = wrap(sd);
            if (!checkValidity(r.shape).valid) return Error("KERNEL_RESULT_INVALID").with("name", std::string("revolve"));
            return r;
        } catch (const Standard_Failure& f) {
            return Error("KERNEL_INTERNAL").with("detail", std::string(f.what() ? f.what() : "Standard_Failure"));
        } catch (...) {
            return Error("KERNEL_INTERNAL").with("detail", std::string("unknown"));
        }
    }

    // ------------------------------------------------------------ booleans (§9.6)
    Result<OpResult> boolean(BoolKind kind, const Shape& target, const Shape& tool, const Cancel* cancel) const override {
        auto r = booleanAttempts(kind, target, tool, cancel);
        if (r || kind != BoolKind::Union || r.error().code != "KERNEL_RESULT_INVALID") return r;
        // Union fallback (§9.6): A ∪ B = A ∪ (B \ A) — the tool without the part inside the target touches the target only
        // along faces; the history is composed through the subtraction.
        auto cut = booleanAttempts(BoolKind::Subtract, tool, target, cancel);
        if (!cut) return r;
        if (D(cut->shape).solids.empty()) {  // the tool is inside the target
            OpResult out;
            out.shape = target;
            out.history.faceOrigins.assign(D(target).faces.Extent(), {});
            for (int f = 0; f < D(target).faces.Extent(); ++f) {
                Origin o;
                o.kind = Origin::Kind::FromFace;
                o.input = {0, f};
                out.history.faceOrigins[size_t(f)].push_back(o);
            }
            for (int f = 0; f < D(tool).faces.Extent(); ++f) out.history.deletedFaces.push_back({1, f});
            out.warnings = cut->warnings;
            return out;
        }
        auto u = booleanAttempts(BoolKind::Union, target, cut->shape, cancel);
        if (!u) return r;
        // Faces of the union from the cut (operand 1) — to their origins in the cut: its operand 0 is the tool (1), its
        // operand 1 — the target (0).
        OpResult out;
        out.shape = u->shape;
        out.warnings = cut->warnings;
        out.warnings.insert(out.warnings.end(), u->warnings.begin(), u->warnings.end());
        out.history.faceOrigins.resize(u->history.faceOrigins.size());
        std::set<std::pair<int, int>> used;
        for (size_t f = 0; f < u->history.faceOrigins.size(); ++f)
            for (const Origin& o : u->history.faceOrigins[f]) {
                if (o.kind != Origin::Kind::FromFace || o.input.operand == 0) {
                    out.history.faceOrigins[f].push_back(o);
                    if (o.kind == Origin::Kind::FromFace) used.insert({0, o.input.face});
                    continue;
                }
                const auto& via = cut->history.faceOrigins.at(size_t(o.input.face));
                for (Origin c : via) {
                    if (c.kind != Origin::Kind::FromFace) continue;
                    c.input.operand = c.input.operand == 0 ? 1 : 0;
                    c.modified = c.modified || o.modified;
                    used.insert({c.input.operand, c.input.face});
                    out.history.faceOrigins[f].push_back(c);
                }
            }
        for (int operand = 0; operand < 2; ++operand)
            for (int f = 0; f < D(operand == 0 ? target : tool).faces.Extent(); ++f)
                if (!used.count({operand, f})) out.history.deletedFaces.push_back({operand, f});
        return out;
    }

    Result<OpResult> booleanAttempts(BoolKind kind, const Shape& target, const Shape& tool, const Cancel* cancel) const {
        const char* name = kind == BoolKind::Union ? "union" : kind == BoolKind::Subtract ? "subtract" : "intersect";
        if (target.null() || tool.null() || D(target).solids.empty() || D(tool).solids.empty())
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("operand"));
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        // Attempts (§9.6): exact, fuzzy KERNEL_BOOLEAN_FUZZY; for a union or intersection the same with the operands
        // swapped (the argument and the tool — the result of OCCT on tangent or coincident faces depends on the order); then
        // KERNEL_BOOLEAN_FUZZY_MAX. Example (FX7-PT-005): a mirror copy of a tube touching the mirror plane — the tube and
        // its copy touch along a circle; one order gives an edge with a tolerance of 20 mm, the other a valid body.
        struct Attempt {
            double fuzzy;
            bool swap;
        };
        const bool symmetric = kind != BoolKind::Subtract;
        std::vector<Attempt> attempts = {{0.0, false}, {KERNEL_BOOLEAN_FUZZY, false}};
        if (symmetric) attempts.insert(attempts.end(), {{0.0, true}, {KERNEL_BOOLEAN_FUZZY, true}});
        attempts.push_back({KERNEL_BOOLEAN_FUZZY_MAX, false});
        bool invalid = false;
        for (size_t attempt = 0; attempt < attempts.size(); ++attempt) {
            if (attempt > 0 && cancelled(cancel)) return Error("KERNEL_CANCELLED");
            try {
                std::unique_ptr<BRepAlgoAPI_BooleanOperation> op;
                if (kind == BoolKind::Union) op = std::make_unique<BRepAlgoAPI_Fuse>();
                else if (kind == BoolKind::Subtract) op = std::make_unique<BRepAlgoAPI_Cut>();
                else op = std::make_unique<BRepAlgoAPI_Common>();
                ShapeList args, tools;
                for (const auto& s : D(target).solids) args.Append(s);
                for (const auto& s : D(tool).solids) tools.Append(s);
                if (attempts[attempt].swap) std::swap(args, tools);
                op->SetArguments(args);
                op->SetTools(tools);
                op->SetRunParallel(false);
                op->SetNonDestructive(true);
                op->SetGlue(BOPAlgo_GlueOff);
                op->SetCheckInverted(true);
                op->SetToFillHistory(true);
                op->SetFuzzyValue(attempts[attempt].fuzzy);
                op->Build();
                if (op->HasErrors() || !op->IsDone()) continue;
                const TopoDS_Shape res = op->Shape();
                if (TopExp_Explorer(res, TopAbs_FACE, TopAbs_SOLID).More()) {  // faces outside solids
                    invalid = true;
                    continue;
                }
                auto sd = makeShapeData(solidsOf(res));
                if (kind == BoolKind::Union && sd->solids.empty()) {  // a union of solids is never empty
                    invalid = true;
                    continue;
                }
                OpResult r;
                r.history.faceOrigins.assign(sd->faces.Extent(), {});
                const OcctShape* inputs[2] = {&D(target), &D(tool)};
                for (int operand = 0; operand < 2; ++operand)
                    for (int f = 1; f <= inputs[operand]->faces.Extent(); ++f) {
                        const TopoDS_Shape& F = inputs[operand]->faces(f);
                        bool used = false;
                        if (!op->IsDeleted(F)) {
                            for (const TopoDS_Shape& m : op->Modified(F)) {
                                const int fi = sd->faces.FindIndex(m);
                                if (fi <= 0) continue;
                                Origin o;
                                o.kind = Origin::Kind::FromFace;
                                o.input = {operand, f - 1};
                                o.modified = true;
                                r.history.faceOrigins[fi - 1].push_back(o);
                                used = true;
                            }
                            if (!used) {
                                const int fi = sd->faces.FindIndex(F);
                                if (fi > 0) {
                                    Origin o;
                                    o.kind = Origin::Kind::FromFace;
                                    o.input = {operand, f - 1};
                                    o.modified = false;
                                    r.history.faceOrigins[fi - 1].push_back(o);
                                    used = true;
                                }
                            }
                        }
                        if (!used) r.history.deletedFaces.push_back({operand, f - 1});
                    }
                recoverHistory(*sd, inputs, 2, r);
                r.shape = wrap(sd);
                if (!checkValidity(r.shape).valid) {
                    invalid = true;
                    continue;
                }
                if (attempts[attempt].fuzzy > 0) r.warnings.push_back(Error("KERNEL_BOOLEAN_FUZZY_RETRY").with("name", std::string(name)));
                const double tol = maxTolerance(sd->shape);
                const double inTol = std::max(maxTolerance(D(target).shape), maxTolerance(D(tool).shape));
                if (toleranceGrew(tol, inTol)) r.warnings.push_back(Error("KERNEL_TOLERANCE_GROWTH").with("actual", tol));
                return r;
            } catch (const Standard_Failure&) {
                continue;
            } catch (...) {
                continue;
            }
        }
        if (invalid) return Error("KERNEL_RESULT_INVALID").with("name", std::string(name));
        return Error("KERNEL_BOOLEAN_FAILED").with("name", std::string(name));
    }

    // Recovering incomplete history from the carrier surface (§9.10).
    static void recoverHistory(const OcctShape& sd, const OcctShape* const* inputs, int nInputs, OpResult& r) {
        int missing = 0;
        for (int fi = 1; fi <= sd.faces.Extent(); ++fi) {
            auto& o = r.history.faceOrigins[fi - 1];
            if (o.empty()) {
                const auto surf = BRep_Tool::Surface(TopoDS::Face(sd.faces(fi)));
                for (int k = 0; k < nInputs; ++k)
                    for (int f = 1; f <= inputs[k]->faces.Extent(); ++f)
                        if (BRep_Tool::Surface(TopoDS::Face(inputs[k]->faces(f))).get() == surf.get()) {
                            Origin x;
                            x.kind = Origin::Kind::FromFace;
                            x.input = {k, f - 1};
                            x.modified = true;
                            o.push_back(x);
                        }
                if (o.empty()) ++missing;
            }
            sortOrigins(o);
        }
        if (missing) r.warnings.push_back(Error("KERNEL_HISTORY_INCOMPLETE").with("actual", static_cast<double>(missing)));
    }

    // ------------------------------------------------------------ fillet/chamfer (§9.7, 1б)
    template <class Maker>
    Result<OpResult> edgeOp(const Shape& body, const std::vector<EdgeRef>& edges, double size, bool isFillet,
                            const Cancel* cancel) const {
        const char* sizeName = isFillet ? "radius" : "distance";
        if (edges.empty()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("edges"));
        const CanonicalTopology ct = canonicalTopology(*this, body);
        for (const auto& e : edges)
            if (e.edge < 0 || e.edge >= static_cast<int>(ct.logicalEdges.size()))
                return Error("KERNEL_INVALID_INDEX").with("name", std::string("logicalEdge")).with("actual", static_cast<double>(e.edge));
        // Common size and per-edge sizes (EdgeRef.size > 0, FX5-FEAT-001) — the same range.
        std::vector<double> sizes{size};
        for (const auto& e : edges) {
            if (e.size > 0) sizes.push_back(e.size);
            if (!isFillet && e.size2 > 0) sizes.push_back(e.size2);
            if (!isFillet && e.angle != 0 && !(e.angle >= KERNEL_MIN_ANGLE && e.angle <= PI / 2 - KERNEL_MIN_ANGLE))
                return Error("KERNEL_INVALID_DIMENSION").with("name", std::string("angle")).with("actual", e.angle).with("expected", PI / 4);
        }
        for (double v : sizes)
            if (!(v >= KERNEL_MIN_DIMENSION && v <= KERNEL_MAX_COORD)) {
                Error e("KERNEL_INVALID_DIMENSION");
                e.with("name", std::string(sizeName)).with("actual", v).with("expected", KERNEL_MIN_DIMENSION);
                return e;
            }
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        const OcctShape& d = D(body);
        std::vector<std::string> labels;
        for (const auto& e : edges) labels.push_back(e.label);
        // §3.5: a tangent edge (smooth junction of faces) is not filleted: the normals of the adjacent faces at the
        // edge midpoint coincide within KERNEL_TANGENT_EDGE_ANGLE.
        std::vector<std::string> tangent;
        for (const auto& e : edges) {
            const LogicalEdge& L = ct.logicalEdges[e.edge];
            BRepAdaptor_Curve mc(TopoDS::Edge(d.edges(L.edges[L.edges.size() / 2] + 1)));
            const Point3 m = pt(mc.Value((mc.FirstParameter() + mc.LastParameter()) / 2));
            const Vec3 n1 = faceNormalAt(body, L.faces.first, m), n2 = faceNormalAt(body, L.faces.second, m);
            const double ang = std::acos(std::clamp(n1.normalized().dot(n2.normalized()), -1.0, 1.0));
            if (ang <= KERNEL_TANGENT_EDGE_ANGLE) tangent.push_back(e.label);
        }
        if (!tangent.empty()) return Error("KERNEL_EDGE_NOT_FILLETABLE").with("ids", tangent);
        try {
            Maker mk(d.shape);
            std::set<int> done;
            for (const auto& e : edges) {
                if (!done.insert(e.edge).second) continue;
                const double sz = e.size > 0 ? e.size : size;
                const LogicalEdge& L = ct.logicalEdges[e.edge];
                for (int ke : L.edges) {
                    if constexpr (std::is_same_v<Maker, BRepFilletAPI_MakeChamfer>) {
                        // Asymmetric chamfer (§3.6, FX7-PT-010): sizes measured from the reference face.
                        const TopoDS_Face ref = TopoDS::Face(d.faces((e.flip ? L.faces.second : L.faces.first) + 1));
                        if (e.angle > 0) {
                            mk.AddDA(sz, e.angle, TopoDS::Edge(d.edges(ke + 1)), ref);
                            continue;
                        }
                        if (e.size2 > 0) {
                            mk.Add(sz, e.size2, TopoDS::Edge(d.edges(ke + 1)), ref);
                            continue;
                        }
                    }
                    mk.Add(sz, TopoDS::Edge(d.edges(ke + 1)));
                }
            }
            mk.Build();
            if (!mk.IsDone()) return Error(isFillet ? "KERNEL_FILLET_FAILED" : "KERNEL_CHAMFER_FAILED").with("ids", std::vector<std::string>{});
            auto sd = makeShapeData(solidsOf(mk.Shape()));
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            for (int f = 1; f <= d.faces.Extent(); ++f) {
                const TopoDS_Shape& F = d.faces(f);
                bool used = false;
                if (!mk.IsDeleted(F)) {
                    for (const TopoDS_Shape& m : mk.Modified(F)) {
                        const int fi = sd->faces.FindIndex(m);
                        if (fi <= 0) continue;
                        Origin o;
                        o.kind = Origin::Kind::FromFace;
                        o.input = {0, f - 1};
                        o.modified = true;
                        r.history.faceOrigins[fi - 1].push_back(o);
                        used = true;
                    }
                    if (!used) {
                        const int fi = sd->faces.FindIndex(F);
                        if (fi > 0) {
                            Origin o;
                            o.kind = Origin::Kind::FromFace;
                            o.input = {0, f - 1};
                            r.history.faceOrigins[fi - 1].push_back(o);
                            used = true;
                        }
                    }
                }
                if (!used) r.history.deletedFaces.push_back({0, f - 1});
            }
            // Fillet faces: Generated(edge) → logical edges of the input.
            std::map<int, std::set<int>> fromEdges;
            for (size_t le = 0; le < ct.logicalEdges.size(); ++le)
                for (int ke : ct.logicalEdges[le].edges)
                    for (const TopoDS_Shape& g : mk.Generated(d.edges(ke + 1))) {
                        const int fi = sd->faces.FindIndex(g);
                        if (fi > 0) fromEdges[fi - 1].insert(static_cast<int>(le));
                    }
            for (auto& [fi, les] : fromEdges) {
                Origin o;
                o.kind = Origin::Kind::FromEdge;
                o.operand = 0;
                o.edges.assign(les.begin(), les.end());
                r.history.faceOrigins[fi].push_back(o);
            }
            for (int v : ct.essentialVertices)
                for (const TopoDS_Shape& g : mk.Generated(d.vertices(v + 1))) {
                    const int fi = sd->faces.FindIndex(g);
                    if (fi <= 0) continue;
                    Origin o;
                    o.kind = Origin::Kind::FromVertex;
                    o.vertex = v;
                    r.history.faceOrigins[fi - 1].push_back(o);
                }
            const OcctShape* inputs[1] = {&d};
            recoverHistory(*sd, inputs, 1, r);
            r.shape = wrap(sd);
            if (!checkValidity(r.shape).valid)
                return Error("KERNEL_RESULT_INVALID").with("name", std::string(isFillet ? "fillet" : "chamfer"));
            return r;
        } catch (...) {
            return Error(isFillet ? "KERNEL_FILLET_FAILED" : "KERNEL_CHAMFER_FAILED").with("ids", std::vector<std::string>{});
        }
    }

    Result<OpResult> fillet(const Shape& body, const std::vector<EdgeRef>& edges, double radius, const Cancel* c) const override {
        return edgeOp<BRepFilletAPI_MakeFillet>(body, edges, radius, true, c);
    }
    Result<OpResult> chamfer(const Shape& body, const std::vector<EdgeRef>& edges, double dist, const Cancel* c) const override {
        return edgeOp<BRepFilletAPI_MakeChamfer>(body, edges, dist, false, c);
    }

    // ------------------------------------------------------------ tessellation (§9.8)
    Result<Tessellation> tessellate(const Shape& body, const TessParams& prm, const Cancel* cancel) const override {
        if (!(prm.chordTol >= KERNEL_MIN_CHORD && prm.chordTol <= KERNEL_MAX_CHORD))
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol"));
        if (!(prm.angleTol >= KERNEL_MIN_ANGLE_TOL && prm.angleTol <= PI / 2))
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("angleTol"));
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        const OcctShape& orig = D(body);
        Tessellation out;
        try {
            BRepBuilderAPI_Copy copier(orig.shape, false, false);
            TopoDS_Shape copy = copier.Shape();
            BRepTools::Clean(copy);
            auto cd = makeShapeData(solidsOf(copy));  // same structure → same indices
            IMeshTools_Parameters mp;
            mp.Deflection = prm.chordTol;
            mp.Angle = prm.angleTol;
            mp.Relative = false;
            mp.InParallel = false;
            mp.MinSize = 0.1 * prm.chordTol;
            mp.ControlSurfaceDeflection = true;
            mp.InternalVerticesMode = true;
            BRepMesh_IncrementalMesh mesh(cd->shape, mp);
            const int F = cd->faces.Extent();
            std::vector<occ::handle<Poly_Triangulation>> tris(F);
            std::vector<TopLoc_Location> locs(F);
            out.faces.resize(F);
            for (int f = 0; f < F; ++f) {
                const TopoDS_Face& face = TopoDS::Face(cd->faces(f + 1));
                FaceMesh& fm = out.faces[f];
                fm.face = f;
                tris[f] = BRep_Tool::Triangulation(face, locs[f]);
                if (tris[f].IsNull() || tris[f]->NbTriangles() == 0) {
                    out.failedFaces.push_back(f);
                    continue;
                }
                const auto& tri = tris[f];
                const gp_Trsf tr = locs[f].Transformation();
                const bool rev = face.Orientation() == TopAbs_REVERSED;
                BRepAdaptor_Surface surf(face, false);
                for (int i = 1; i <= tri->NbNodes(); ++i) {
                    const gp_Pnt p = tri->Node(i).Transformed(tr);
                    fm.positions.insert(fm.positions.end(), {float(p.X()), float(p.Y()), float(p.Z())});
                    gp_Vec nrm(0, 0, 1);
                    if (tri->HasUVNodes()) {
                        gp_Pnt q;
                        gp_Vec du, dv;
                        const gp_Pnt2d uv = tri->UVNode(i);
                        surf.D1(uv.X(), uv.Y(), q, du, dv);
                        nrm = du.Crossed(dv);
                        if (nrm.Magnitude() > 1e-300) nrm.Normalize();
                        nrm.Transform(tr);
                        if (rev) nrm.Reverse();
                    }
                    fm.normals.insert(fm.normals.end(), {float(nrm.X()), float(nrm.Y()), float(nrm.Z())});
                }
                for (int t = 1; t <= tri->NbTriangles(); ++t) {
                    int a, b, c;
                    tri->Triangle(t).Get(a, b, c);
                    if (rev) std::swap(b, c);
                    fm.triangles.insert(fm.triangles.end(), {uint32_t(a - 1), uint32_t(b - 1), uint32_t(c - 1)});
                }
            }
            // Kernel edge polylines and alignment of face nodes (watertightness, §3.7 item 3).
            const int E = cd->edges.Extent();
            std::vector<std::vector<float>> polylines(E);
            for (int e = 0; e < E; ++e) {
                const TopoDS_Edge edge = TopoDS::Edge(cd->edges(e + 1));
                std::vector<int> fs = cd->edgeFaces[e];
                std::sort(fs.begin(), fs.end());
                fs.erase(std::unique(fs.begin(), fs.end()), fs.end());
                if (fs.empty()) continue;
                const int f0 = fs.front();
                if (tris[f0].IsNull()) continue;
                const auto poly = BRep_Tool::PolygonOnTriangulation(edge, tris[f0], locs[f0]);
                if (poly.IsNull()) continue;
                std::vector<float>& pl = polylines[e];
                const FaceMesh& fm0 = out.faces[f0];
                for (int k = 1; k <= poly->NbNodes(); ++k) {
                    const int ni = poly->Node(k) - 1;
                    pl.insert(pl.end(), {fm0.positions[3 * ni], fm0.positions[3 * ni + 1], fm0.positions[3 * ni + 2]});
                }
                for (int f : fs) {
                    if (tris[f].IsNull()) continue;
                    for (TopAbs_Orientation ori : {TopAbs_FORWARD, TopAbs_REVERSED}) {
                        const auto pg = BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(edge.Oriented(ori)), tris[f], locs[f]);
                        if (pg.IsNull() || pg->NbNodes() * 3 != static_cast<int>(pl.size())) continue;
                        FaceMesh& fm = out.faces[f];
                        for (int k = 1; k <= pg->NbNodes(); ++k) {
                            const int ni = pg->Node(k) - 1;
                            for (int c = 0; c < 3; ++c) fm.positions[3 * ni + c] = pl[3 * (k - 1) + c];
                        }
                    }
                }
            }
            const CanonicalTopology ct = canonicalTopology(*this, body);
            for (size_t le = 0; le < ct.logicalEdges.size(); ++le) {
                LogicalEdgeMesh lm;
                lm.edge = static_cast<int>(le);
                for (int ke : ct.logicalEdges[le].edges)
                    if (polylines[ke].size() >= 6) lm.polylines.push_back(polylines[ke]);
                out.edges.push_back(lm);
            }
            for (int v : ct.essentialVertices) {
                const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(orig.vertices(v + 1)));
                out.vertices.push_back({v, {float(p.X()), float(p.Y()), float(p.Z())}});
            }
            return out;
        } catch (const Standard_Failure& f) {
            return Error("KERNEL_INTERNAL").with("detail", std::string(f.what() ? f.what() : "Standard_Failure"));
        } catch (...) {
            return Error("KERNEL_INTERNAL").with("detail", std::string("unknown"));
        }
    }

    // ------------------------------------------------------------ raw topology
    Counts counts(const Shape& s) const override {
        const OcctShape& d = D(s);
        return {static_cast<int>(d.solids.size()), d.faces.Extent(), d.edges.Extent(), d.vertices.Extent()};
    }
    std::vector<int> solidFaces(const Shape& s, int k) const override {
        const OcctShape& d = D(s);
        std::set<int> out;
        for (TopExp_Explorer ex(d.solids.at(k), TopAbs_FACE); ex.More(); ex.Next()) out.insert(d.faces.FindIndex(ex.Current()) - 1);
        return {out.begin(), out.end()};
    }
    std::vector<int> faceEdges(const Shape& s, int f) const override {
        const OcctShape& d = D(s);
        std::set<int> out;
        for (TopExp_Explorer ex(d.faces(f + 1), TopAbs_EDGE); ex.More(); ex.Next()) {
            const int e = d.edges.FindIndex(ex.Current());
            if (e > 0) out.insert(e - 1);
        }
        return {out.begin(), out.end()};
    }
    std::pair<int, int> edgeFaces(const Shape& s, int e) const override {
        const auto& l = D(s).edgeFaces.at(e);
        if (l.empty()) return {-1, -1};
        if (l.size() == 1) return {l[0], l[0]};
        return {std::min(l[0], l[1]), std::max(l[0], l[1])};
    }
    std::pair<int, int> edgeVertices(const Shape& s, int e) const override { return D(s).edgeVerts.at(e); }
    std::vector<int> vertexFaces(const Shape& s, int v) const override { return D(s).vertexFaces.at(v); }
    std::vector<int> vertexEdges(const Shape& s, int v) const override { return D(s).vertexEdges.at(v); }

    // ------------------------------------------------------------ measurements
    MassProperties massProperties(const Shape& s) const override {
        MassProperties m;
        const OcctShape& d = D(s);
        if (d.solids.empty()) return m;
        std::lock_guard lk(d.memo->mu);
        if (d.memo->mass) return *d.memo->mass;
        const GProp_GProps v = volumeProps(d.shape), a = surfaceProps(d.shape);
        m.volume = v.Mass();
        m.area = a.Mass();
        m.centroid = pt(v.CentreOfMass());
        const gp_Mat I = v.MatrixOfInertia();  // relative to the center of mass
        for (int r = 0; r < 3; ++r)
            for (int q = 0; q < 3; ++q) m.inertia(r, q) = I.Value(r + 1, q + 1);
        d.memo->mass = m;
        return m;
    }
    BBox3 bbox(const Shape& s) const override {
        BBox3 b;
        const OcctShape& d = D(s);
        if (d.solids.empty()) return b;
        std::lock_guard lk(d.memo->mu);
        if (d.memo->bbox) return *d.memo->bbox;
        Bnd_Box box;
        BRepBndLib::AddOptimal(d.shape, box, false, false);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        b.extend(Point3(x0, y0, z0));
        b.extend(Point3(x1, y1, z1));
        d.memo->bbox = b;
        return b;
    }
    double faceArea(const Shape& s, int f) const override {
        return faceProps(D(s)).at(size_t(f)).first;
    }
    Point3 faceCentroid(const Shape& s, int f) const override {
        return faceProps(D(s)).at(size_t(f)).second;
    }
    double edgeLength(const Shape& s, int e) const override {
        GProp_GProps g;
        BRepGProp::LinearProperties(D(s).edges(e + 1), g);
        return g.Mass();
    }
    Point3 edgeCentroid(const Shape& s, int e) const override {
        GProp_GProps g;
        BRepGProp::LinearProperties(D(s).edges(e + 1), g);
        return pt(g.CentreOfMass());
    }
    Point3 edgeMidpoint(const Shape& s, int e) const override {
        BRepAdaptor_Curve c(TopoDS::Edge(D(s).edges(e + 1)));
        return pt(c.Value((c.FirstParameter() + c.LastParameter()) / 2));
    }
    Point3 vertexPoint(const Shape& s, int v) const override { return pt(BRep_Tool::Pnt(TopoDS::Vertex(D(s).vertices(v + 1)))); }

    static Vec3 outwardNormal(const TopoDS_Face& face, double u, double v) {
        BRepAdaptor_Surface surf(face, false);
        gp_Pnt q;
        gp_Vec du, dv;
        surf.D1(u, v, q, du, dv);
        gp_Vec n = du.Crossed(dv);
        if (n.Magnitude() > 1e-300) n.Normalize();
        if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
        return {n.X(), n.Y(), n.Z()};
    }

    Vec3 faceNormalAt(const Shape& s, int f, const Point3& p) const override {
        const TopoDS_Face& face = TopoDS::Face(D(s).faces(f + 1));
        GeomAPI_ProjectPointOnSurf proj(gp(p), BRep_Tool::Surface(face));
        double u = 0, v = 0;
        if (proj.NbPoints() > 0) proj.LowerDistanceParameters(u, v);
        return outwardNormal(face, u, v);
    }
    double distanceToFace(const Shape& s, int f, const Point3& p) const override {
        BRepExtrema_DistShapeShape ds(BRepBuilderAPI_MakeVertex(gp(p)).Vertex(), D(s).faces(f + 1));
        return ds.IsDone() ? ds.Value() : 1e300;
    }
    Point3 closestPointOnFace(const Shape& s, int f, const Point3& p) const override {
        BRepExtrema_DistShapeShape ds(BRepBuilderAPI_MakeVertex(gp(p)).Vertex(), D(s).faces(f + 1));
        if (!ds.IsDone() || ds.NbSolution() < 1) return p;
        return pt(ds.PointOnShape2(1));
    }
    double distanceToEdge(const Shape& s, int e, const Point3& p) const override {
        BRepExtrema_DistShapeShape ds(BRepBuilderAPI_MakeVertex(gp(p)).Vertex(), D(s).edges(e + 1));
        return ds.IsDone() ? ds.Value() : 1e300;
    }
    // kernel-interface.md §3.21, §9.9.
    double distanceToShape(const Shape& s, const Point3& p) const override {
        if (s.null() || D(s).shape.IsNull() || D(s).faces.Extent() == 0) return std::numeric_limits<double>::infinity();
        BRepExtrema_DistShapeShape ds(BRepBuilderAPI_MakeVertex(gp(p)).Vertex(), D(s).shape);
        return ds.IsDone() && ds.NbSolution() > 0 ? ds.Value() : std::numeric_limits<double>::infinity();
    }
    // kernel-interface.md §3.8 rayHitsShape.
    bool rayHitsShape(const Shape& s, const Point3& p, const Vec3& d, double tMin) const override {
        if (s.null() || D(s).shape.IsNull()) return false;
        IntCurvesFace_ShapeIntersector inter;
        inter.Load(D(s).shape, Precision::Confusion());
        inter.Perform(gp_Lin(gp(p), gp_Dir(d.x(), d.y(), d.z())), tMin, Precision::Infinite());
        return inter.IsDone() && inter.NbPnt() > 0;
    }

    static Vec3 canonicalDir(Vec3 d) {
        for (int i = 0; i < 3; ++i) {
            if (std::abs(d[i]) > KERNEL_DIR_SIGN_TOL) {
                if (d[i] < 0) d = -d;
                break;
            }
        }
        return d;
    }

    SurfaceInfo faceSurface(const Shape& s, int f) const override {
        SurfaceInfo si;
        const TopoDS_Face& face = TopoDS::Face(D(s).faces(f + 1));
        BRepAdaptor_Surface surf(face, true);
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        const double um = (u0 + u1) / 2, vm = (v0 + v1) / 2;
        const Vec3 nOut = outwardNormal(face, um, vm);
        gp_Pnt pm;
        gp_Vec du, dv;
        surf.D1(um, vm, pm, du, dv);
        const Point3 P = pt(pm);
        auto projOnAxis = [](const Point3& o, const Vec3& a, const Point3& q) { return Point3(o + a * (q - o).dot(a)); };
        switch (surf.GetType()) {
            case GeomAbs_Plane: {
                si.kind = SurfaceInfo::Kind::Plane;
                si.dir = nOut;
                const Point3 o = pt(surf.Plane().Location());
                si.point = Point3::Zero() - nOut * (Point3::Zero() - o).dot(nOut);
                break;
            }
            case GeomAbs_Cylinder: {
                const gp_Cylinder c = surf.Cylinder();
                si.kind = SurfaceInfo::Kind::Cylinder;
                si.dir = canonicalDir(vc(c.Axis().Direction()));
                si.point = projOnAxis(pt(c.Location()), si.dir, Point3::Zero());
                si.radius = c.Radius();
                si.convex = nOut.dot(P - projOnAxis(si.point, si.dir, P)) > 0;
                break;
            }
            case GeomAbs_Cone: {
                const gp_Cone c = surf.Cone();
                si.kind = SurfaceInfo::Kind::Cone;
                si.point = pt(c.Apex());
                Vec3 a = vc(c.Axis().Direction());
                if ((P - si.point).dot(a) < 0) a = -a;
                si.dir = a;
                si.halfAngle = std::abs(c.SemiAngle());
                si.convex = nOut.dot(P - projOnAxis(si.point, si.dir, P)) > 0;
                break;
            }
            case GeomAbs_Sphere: {
                const gp_Sphere sp = surf.Sphere();
                si.kind = SurfaceInfo::Kind::Sphere;
                si.point = pt(sp.Location());
                si.radius = sp.Radius();
                si.convex = nOut.dot(P - si.point) > 0;
                break;
            }
            case GeomAbs_Torus: {
                const gp_Torus t = surf.Torus();
                si.kind = SurfaceInfo::Kind::Torus;
                si.point = pt(t.Location());
                si.dir = canonicalDir(vc(t.Axis().Direction()));
                si.radius = t.MajorRadius();
                si.minorRadius = t.MinorRadius();
                Vec3 radial = P - projOnAxis(si.point, si.dir, P);
                if (radial.norm() > 0) radial.normalize();
                const Point3 tubeCenter = si.point + radial * si.radius + si.dir * (P - si.point).dot(si.dir);
                si.convex = nOut.dot(P - tubeCenter) > 0;
                break;
            }
            case GeomAbs_SurfaceOfRevolution: {  // FX7-DRW-010: axis like a cylinder's
                const gp_Ax1 ax = surf.AxeOfRevolution();
                si.kind = SurfaceInfo::Kind::Other;
                si.otherKind = "revolution";
                si.dir = canonicalDir(vc(ax.Direction()));
                si.point = projOnAxis(pt(ax.Location()), si.dir, Point3::Zero());
                break;
            }
            default:
                si.kind = SurfaceInfo::Kind::Other;
                si.otherKind = "bspline";
                break;
        }
        return si;
    }

    // ---------------------------------------------------------------- curves of point sets (§3.8, §9.9, FX7-DRW-010)

    // One parametric curve of a set: point by parameter, the parameter range split at continuity breaks, and the
    // membership of a parameter (the meridian — only points of the face).
    struct SetCurve {
        std::function<gp_Pnt(double)> value;
        std::vector<double> breaks;  // increasing, the first and the last — the range
        std::function<bool(double)> inside;
    };

    Result<std::vector<SetCurve>> setCurves(const Shape& s, const CurveSet& q) const {
        const OcctShape& d = D(s);
        std::vector<SetCurve> out;
        if ((q.edge >= 0) == (q.face >= 0)) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("curveSet"));
        if (q.edge >= 0) {
            const CanonicalTopology ct = canonicalTopology(*this, s);
            if (q.edge >= static_cast<int>(ct.logicalEdges.size()))
                return Error("KERNEL_INVALID_INDEX").with("name", std::string("logicalEdge")).with("actual", static_cast<double>(q.edge));
            for (int ke : ct.logicalEdges.at(size_t(q.edge)).edges) {
                auto c = std::make_shared<BRepAdaptor_Curve>(TopoDS::Edge(d.edges(ke + 1)));
                SetCurve sc;
                sc.value = [c](double t) { return c->Value(t); };
                const int n = c->NbIntervals(GeomAbs_C2);
                TColStd_Array1OfReal iv(1, n + 1);
                c->Intervals(iv, GeomAbs_C2);
                for (int i = 1; i <= n + 1; ++i) sc.breaks.push_back(iv(i));
                sc.inside = [](double) { return true; };
                out.push_back(std::move(sc));
            }
            return out;
        }
        if (q.face >= d.faces.Extent())
            return Error("KERNEL_INVALID_INDEX").with("name", std::string("face")).with("actual", static_cast<double>(q.face));
        const Vec3 h = q.half;
        if (!h.allFinite() || std::abs(h.norm() - 1) > 1e-9) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("half"));
        const TopoDS_Face face = TopoDS::Face(d.faces(q.face + 1));
        auto surf = std::make_shared<BRepAdaptor_Surface>(face, true);
        gp_Ax1 ax;
        switch (surf->GetType()) {
            case GeomAbs_Cylinder: ax = surf->Cylinder().Axis(); break;
            case GeomAbs_Cone: ax = surf->Cone().Axis(); break;
            case GeomAbs_Torus: ax = surf->Torus().Axis(); break;
            case GeomAbs_SurfaceOfRevolution: ax = surf->AxeOfRevolution(); break;
            default: return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("surfaceType"));
        }
        const Vec3 a = vc(ax.Direction());
        const Point3 A = pt(ax.Location());
        if (std::abs(h.dot(a)) > KERNEL_AXIS_ANGLE_TOL) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("half"));
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        // Rotation angle of the half-plane: from the radial direction of the point at u = 0 about the axis direction.
        Vec3 e0 = Vec3::Zero();
        for (double f : {0.5, 0.25, 0.75, 0.1, 0.9}) {
            const Point3 P = pt(surf->Value(0, v0 + f * (v1 - v0)));
            e0 = (P - A) - a * (P - A).dot(a);
            if (e0.norm() > LINEAR_TOL) break;
        }
        if (e0.norm() <= LINEAR_TOL) return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("meridian"));
        e0.normalize();
        double uh = std::atan2(e0.cross(h).dot(a), e0.dot(h));
        while (uh < u0 - 1e-9) uh += 2 * PI;
        while (uh >= u0 + 2 * PI - 1e-9) uh -= 2 * PI;
        if (uh > u1 + 1e-9) return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("meridian"));
        uh = std::min(uh, u1);
        auto cls = std::make_shared<BRepTopAdaptor_FClass2d>(face, Precision::PConfusion());
        SetCurve sc;
        sc.value = [surf, uh](double v) { return surf->Value(uh, v); };
        const int n = surf->NbVIntervals(GeomAbs_C2);
        TColStd_Array1OfReal iv(1, n + 1);
        surf->VIntervals(iv, GeomAbs_C2);
        for (int i = 1; i <= n + 1; ++i) sc.breaks.push_back(std::clamp(iv(i), v0, v1));
        sc.breaks.front() = v0;
        sc.breaks.back() = v1;
        sc.inside = [cls, uh](double v) { return cls->Perform(gp_Pnt2d(uh, v)) != TopAbs_OUT; };
        out.push_back(std::move(sc));
        return out;
    }

    Result<Point3> extremePoint(const Shape& s, const CurveSet& q, const Vec3& dir) const override {
        if (!dir.allFinite() || dir.norm() <= 0) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("dir"));
        auto curves = setCurves(s, q);
        if (!curves) return curves.error();
        const gp_Vec D(dir.x(), dir.y(), dir.z());
        bool found = false;
        double best = -std::numeric_limits<double>::infinity();
        gp_Pnt bestP;
        for (const SetCurve& sc : *curves) {
            auto f = [&](double t) { return gp_Vec(sc.value(t).XYZ()).Dot(D); };
            // Samples: KERNEL_EXTREME_SAMPLES equal steps on each continuity interval (the ends included).
            std::vector<double> ts;
            for (size_t i = 0; i + 1 < sc.breaks.size(); ++i) {
                const double a = sc.breaks[i], b = sc.breaks[i + 1];
                if (b <= a) continue;
                for (int j = 0; j < KERNEL_EXTREME_SAMPLES; ++j) ts.push_back(a + (b - a) * j / KERNEL_EXTREME_SAMPLES);
            }
            ts.push_back(sc.breaks.back());
            int bi = -1;
            double bv = -std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < ts.size(); ++i) {
                if (!sc.inside(ts[i])) continue;
                const double v = f(ts[i]);
                if (bi < 0 || v > bv) bi = int(i), bv = v;
            }
            if (bi < 0) continue;
            double t = ts[size_t(bi)];
            // Golden-section refinement on the neighbouring steps.
            double lo = ts[size_t(std::max(0, bi - 1))], hi = ts[std::min(ts.size() - 1, size_t(bi) + 1)];
            const double eps = 1e-12 * std::max(1.0, sc.breaks.back() - sc.breaks.front());
            const double g = (std::sqrt(5.0) - 1) / 2;
            double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo), f1 = f(x1), f2 = f(x2);
            for (int it = 0; it < 200 && hi - lo > eps; ++it) {
                if (f1 < f2) {
                    lo = x1, x1 = x2, f1 = f2, x2 = lo + g * (hi - lo), f2 = f(x2);
                } else {
                    hi = x2, x2 = x1, f2 = f1, x1 = hi - g * (hi - lo), f1 = f(x1);
                }
            }
            const double tr = (lo + hi) / 2;
            if (f(tr) > bv && sc.inside(tr)) t = tr;
            const gp_Pnt P = sc.value(t);
            const double v = gp_Vec(P.XYZ()).Dot(D);
            if (!found || v > best) found = true, best = v, bestP = P;
        }
        if (!found) return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("meridian"));
        return pt(bestP);
    }

    Result<std::vector<std::vector<Point3>>> curvePoints(const Shape& s, const CurveSet& q, double deflection) const override {
        if (!std::isfinite(deflection) || deflection <= 0) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("deflection"));
        auto curves = setCurves(s, q);
        if (!curves) return curves.error();
        std::vector<std::vector<Point3>> out;
        for (const SetCurve& sc : *curves) {
            // Adaptive subdivision: a step is split while its middle deviates from the chord by more than deflection.
            std::vector<double> ts;
            std::function<void(double, double, const gp_Pnt&, const gp_Pnt&, int)> split = [&](double a, double b, const gp_Pnt& pa, const gp_Pnt& pb,
                                                                                                 int depth) {
                const double m = (a + b) / 2;
                const gp_Pnt pm = sc.value(m);
                const gp_Vec ab(pa, pb), am(pa, pm);
                const double len = ab.Magnitude();
                const double dev = len > 0 ? am.Crossed(ab).Magnitude() / len : am.Magnitude();
                if (depth < 14 && dev > deflection) {
                    split(a, m, pa, pm, depth + 1);
                    split(m, b, pm, pb, depth + 1);
                } else {
                    ts.push_back(b);
                }
            };
            for (size_t i = 0; i + 1 < sc.breaks.size(); ++i) {
                const double a = sc.breaks[i], b = sc.breaks[i + 1];
                if (b <= a) continue;
                constexpr int START = 8;
                if (ts.empty()) ts.push_back(a);
                for (int j = 0; j < START; ++j) {
                    const double x0 = a + (b - a) * j / START, x1 = a + (b - a) * (j + 1) / START;
                    split(x0, x1, sc.value(x0), sc.value(x1), 0);
                }
            }
            std::vector<Point3> run;
            for (double t : ts) {
                if (sc.inside(t)) {
                    run.push_back(pt(sc.value(t)));
                } else if (!run.empty()) {
                    if (run.size() > 1) out.push_back(std::move(run));
                    run.clear();
                }
            }
            if (run.size() > 1) out.push_back(std::move(run));
        }
        if (out.empty()) return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("meridian"));
        return out;
    }

    CurveInfo edgeCurve(const Shape& s, int le) const override {
        CurveInfo ci;
        const CanonicalTopology ct = canonicalTopology(*this, s);
        const LogicalEdge& L = ct.logicalEdges.at(le);
        const OcctShape& d = D(s);
        bool allLine = true, allCircle = true;
        gp_Circ firstCirc;
        bool haveCirc = false;
        for (int ke : L.edges) {
            BRepAdaptor_Curve c(TopoDS::Edge(d.edges(ke + 1)));
            if (c.GetType() != GeomAbs_Line) allLine = false;
            if (c.GetType() != GeomAbs_Circle) {
                allCircle = false;
            } else {
                const gp_Circ cc = c.Circle();
                if (!haveCirc) {
                    firstCirc = cc;
                    haveCirc = true;
                } else if (cc.Location().Distance(firstCirc.Location()) > LINEAR_TOL ||
                           std::abs(cc.Radius() - firstCirc.Radius()) > LINEAR_TOL) {
                    allCircle = false;
                }
            }
        }
        if (allLine && L.ends.size() == 2) {
            ci.kind = CurveInfo::Kind::Line;
            Point3 a = vertexPoint(s, L.ends[0]), b = vertexPoint(s, L.ends[1]);
            auto less = [](const Point3& x, const Point3& y) {
                for (int i = 0; i < 3; ++i)
                    if (std::abs(x[i] - y[i]) > KERNEL_POINT_ORDER_TOL) return x[i] < y[i];
                return false;
            };
            if (less(b, a)) std::swap(a, b);
            ci.p1 = a;
            ci.p2 = b;
        } else if (allCircle && haveCirc) {
            ci.kind = CurveInfo::Kind::Circle;
            ci.center = pt(firstCirc.Location());
            ci.axis = canonicalDir(vc(firstCirc.Axis().Direction()));
            ci.radius = firstCirc.Radius();
            ci.closed = L.ends.empty();
        } else {
            ci.kind = CurveInfo::Kind::Other;
            ci.otherKind = L.edges.size() > 1 ? "composite" : "other";
        }
        return ci;
    }

    // §3.9 (S4A-SPL-004): a logical edge of one B-spline or Bezier kernel edge projects exactly onto the NURBS with
    // the projected poles (an affine map keeps weights and knots); other curves are not supported.
    template <class UV>
    Result<Curve2> projectSpline(const Shape& s, const LogicalEdge& L, const UV& uv) const {
        auto unsupported = [] { return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("curveType")); };
        if (L.edges.size() != 1) return unsupported();
        const TopoDS_Edge E = TopoDS::Edge(D(s).edges(L.edges[0] + 1));
        BRepAdaptor_Curve ac(E);
        if (ac.GetType() != GeomAbs_BSplineCurve && ac.GetType() != GeomAbs_BezierCurve) return unsupported();
        double f = 0, l = 0;
        const Handle(Geom_Curve) gc = BRep_Tool::Curve(E, f, l);
        if (gc.IsNull() || !(l > f)) return unsupported();
        Handle(Geom_BSplineCurve) b;
        try {
            b = GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(gc, f, l));
            if (!b.IsNull() && b->IsPeriodic()) b->SetNotPeriodic();
        } catch (const Standard_Failure&) {
            return unsupported();
        }
        if (b.IsNull()) return unsupported();
        spline::Curve c;
        c.degree = b->Degree();
        for (int i = 1; i <= b->NbPoles(); ++i) c.poles.push_back(uv(pt(b->Pole(i))));
        if (b->IsRational())
            for (int i = 1; i <= b->NbPoles(); ++i) c.weights.push_back(b->Weight(i));
        for (int i = 1; i <= b->NbKnots(); ++i) {
            c.knots.push_back(b->Knot(i));
            c.mults.push_back(b->Multiplicity(i));
        }
        if (!spline::validate(c).empty()) return unsupported();
        Curve2 out;
        double extent = 0;
        for (const Point2& q : c.poles) extent = std::max(extent, (q - c.poles.front()).norm());
        if (extent <= LINEAR_TOL) {
            out.kind = Curve2::Kind::Point;
            out.p = c.poles.front();
            return out;
        }
        out.kind = Curve2::Kind::Spline;
        out.p1 = c.poles.front();
        out.p2 = c.poles.back();
        out.spline = std::move(c);
        return out;
    }

    // §3.9: orthogonal projection of a logical edge into the plane's local coordinates.
    Result<Curve2> projectEdge(const Shape& s, int le, const Plane& pl) const override {
        const CanonicalTopology ct = canonicalTopology(*this, s);
        if (le < 0 || le >= static_cast<int>(ct.logicalEdges.size()))
            return Error("KERNEL_INVALID_INDEX").with("name", std::string("logicalEdge")).with("actual", static_cast<double>(le));
        auto uv = [&](const Point3& p) { return Point2((p - pl.origin).dot(pl.x), (p - pl.origin).dot(pl.y)); };
        const CurveInfo ci = edgeCurve(s, le);
        Curve2 c;
        if (ci.kind == CurveInfo::Kind::Line) {
            const Point2 a = uv(ci.p1), b = uv(ci.p2);
            if ((b - a).norm() <= LINEAR_TOL) {
                c.kind = Curve2::Kind::Point;
                c.p = a;
            } else {
                c.kind = Curve2::Kind::Line;
                c.p1 = a;
                c.p2 = b;
            }
            return c;
        }
        if (ci.kind == CurveInfo::Kind::Other) return projectSpline(s, ct.logicalEdges.at(le), uv);
        if (ci.kind != CurveInfo::Kind::Circle)
            return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("curveType"));
        const double cosang = std::min(1.0, std::abs(ci.axis.normalized().dot(pl.normal.normalized())));
        if (cosang <= std::sin(ANGULAR_TOL * 1e6)) {
            // Circle seen "edge-on": the projection is the segment uv(center) + r·cosθ·uv₀(d), d = n × axis (§3.9).
            const Vec3 ax = ci.axis.normalized(), n = pl.normal.normalized();
            const Vec3 d = n.cross(ax).normalized(), e2 = ax.cross(d);
            auto theta = [&](const Point3& p) { const Vec3 v = p - ci.center; return std::atan2(v.dot(e2), v.dot(d)); };
            double tmin = -1, tmax = 1;
            if (!ci.closed) {
                const LogicalEdge& L = ct.logicalEdges.at(le);
                const OcctShape& sd = D(s);
                BRepAdaptor_Curve mc(TopoDS::Edge(sd.edges(L.edges[L.edges.size() / 2] + 1)));
                const double ta = theta(vertexPoint(s, L.ends[0])), tb = theta(vertexPoint(s, L.ends[1]));
                const double tm = theta(pt(mc.Value((mc.FirstParameter() + mc.LastParameter()) / 2)));
                auto sweep = [](double from, double to) { return geom2::ccwSweep(from, to); };
                // Sweep: from ta to tb counterclockwise through tm, otherwise from tb to ta.
                double from = ta, len = sweep(ta, tb);
                if (sweep(ta, tm) > len) from = tb, len = sweep(tb, ta);
                tmin = std::min(std::cos(ta), std::cos(tb));
                tmax = std::max(std::cos(ta), std::cos(tb));
                if (sweep(from, 0.0) <= len) tmax = 1;
                if (sweep(from, PI) <= len) tmin = -1;
            }
            const Point2 c0 = uv(ci.center), dir((d).dot(pl.x), (d).dot(pl.y));
            const Point2 a = c0 + ci.radius * tmin * dir, b = c0 + ci.radius * tmax * dir;
            if ((b - a).norm() <= LINEAR_TOL) {
                c.kind = Curve2::Kind::Point;
                c.p = a;
            } else {
                c.kind = Curve2::Kind::Line;
                c.p1 = a;
                c.p2 = b;
            }
            return c;
        }
        if (std::acos(cosang) > ANGULAR_TOL * 1e6) return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("ellipseProjection"));
        c.center = uv(ci.center);
        c.radius = ci.radius;
        if (ci.closed) {
            c.kind = Curve2::Kind::Circle;
            return c;
        }
        const LogicalEdge& L = ct.logicalEdges.at(le);
        Point2 a = uv(vertexPoint(s, L.ends[0])), b = uv(vertexPoint(s, L.ends[1]));
        // Interior point: parameter midpoint of the middle kernel edge of the chain.
        const OcctShape& d = D(s);
        BRepAdaptor_Curve mc(TopoDS::Edge(d.edges(L.edges[L.edges.size() / 2] + 1)));
        const Point2 m = uv(pt(mc.Value((mc.FirstParameter() + mc.LastParameter()) / 2)));
        auto ang = [&](const Point2& q) { return std::atan2(q.y() - c.center.y(), q.x() - c.center.x()); };
        auto sweep = [](double from, double to) { return geom2::ccwSweep(from, to); };
        // The counterclockwise arc a→b must contain m; otherwise b→a.
        if (sweep(ang(a), ang(m)) > sweep(ang(a), ang(b))) std::swap(a, b);
        c.kind = Curve2::Kind::Arc;
        c.start = a;
        c.end = b;
        return c;
    }

    Result<OpResult> transform(const Shape& s, const Motion& m) const override {
        if (s.null() || D(s).solids.empty()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("operand"));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double dot = 0;
                for (int k = 0; k < 3; ++k) dot += m.r[k * 3 + i] * m.r[k * 3 + j];
                if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.0 : 0.0)) > 1e-9)
                    return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("motion"));
            }
        if (!m.t.allFinite()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("motion"));
        try {
            gp_Trsf t;
            t.SetValues(m.r[0], m.r[1], m.r[2], m.t.x(), m.r[3], m.r[4], m.r[5], m.t.y(), m.r[6], m.r[7], m.r[8], m.t.z());
            BRepBuilderAPI_Transform op(D(s).shape, t, true);  // copy: geometry is not shared with the original
            if (!op.IsDone()) return Error("KERNEL_INTERNAL").with("detail", std::string("transform"));
            auto sd = makeShapeData(solidsOf(op.Shape()));
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            const OcctShape& in = D(s);
            for (int f = 1; f <= in.faces.Extent(); ++f) {
                const int fi = sd->faces.FindIndex(op.ModifiedShape(in.faces(f)));
                if (fi <= 0) continue;
                Origin o;
                o.kind = Origin::Kind::FromFace;
                o.input = {0, f - 1};
                r.history.faceOrigins[fi - 1].push_back(o);
            }
            r.shape = wrap(sd);
            return r;
        } catch (const Standard_Failure& f) {
            return Error("KERNEL_INTERNAL").with("detail", std::string(f.what() ? f.what() : "Standard_Failure"));
        }
    }

    // Shell (§3.13, §9.6б): MakeThickSolidByJoin first; for an inward wall, if OCCT did not build the shell or
    // did not build it over the whole body (part of the body stayed solid, FX4-FEAT-002), the fallback "erosion" method
    // (shellByErosion).
    Result<OpResult> shell(const Shape& body, const std::vector<int>& removeFaces, double thickness, bool outward,
                           const Cancel* cancel) const override {
        auto r = shellByJoin(body, removeFaces, thickness, outward, cancel);
        if (outward) return r;
        if (!r && r.error().code != "KERNEL_SHELL_FAILED") return r;
        if (r && !shellIncomplete(body, *r, removeFaces, thickness)) return r;
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        auto e = shellByErosion(body, removeFaces, thickness, cancel);
        if (e) return e;
        return r ? r : e;
    }

    // Completeness check of an inward shell (§9.6б): for each kept face g, a point p on the face (closest to its center
    // of mass), outward normal n. The wall point p − ½·t·n, if inside the body, must be in the result material; the point
    // p − 1½·t·n, if inside the body and at least 1.2·t from every kept face, must be in the cavity. A violation means
    // the shell is incomplete.
    bool shellIncomplete(const Shape& body, const OpResult& res, const std::vector<int>& removeFaces, double t) const {
        const OcctShape& d = D(body);
        const OcctShape& rd = D(res.shape);
        if (rd.solids.size() != 1) return true;
        auto inside = [](const TopoDS_Shape& solid, const Point3& q) {
            BRepClass3d_SolidClassifier cls(solid, gp(q), 1e-7);
            return cls.State() == TopAbs_IN;
        };
        std::vector<int> kept;
        for (int f = 0; f < d.faces.Extent(); ++f)
            if (std::find(removeFaces.begin(), removeFaces.end(), f) == removeFaces.end()) kept.push_back(f);
        for (int f : kept) {
            const auto ip = faceInteriorPoint(TopoDS::Face(d.faces(f + 1)));
            if (!ip || ip->second < 1.5 * t) continue;  // narrow face: the point's neighborhood touches adjacent faces
            const Point3 p = ip->first;
            const Vec3 n = faceNormalAt(body, f, p);
            const Point3 wall = p - 0.5 * t * n, deep = p - 1.5 * t * n;
            if (inside(d.solids.front(), wall) && !inside(rd.solids.front(), wall)) return true;
            if (!inside(d.solids.front(), deep)) continue;
            bool remote = true;
            for (int g : kept)
                if (distanceToFace(body, g, deep) < 1.2 * t) {
                    remote = false;
                    break;
                }
            if (remote && inside(rd.solids.front(), deep)) return true;
        }
        return false;
    }

    // A point inside the face and its distance to the face boundary (the largest among the nodes of a 5×5 grid over the
    // parameter domain that lie inside the face); nullopt means no node is inside.
    static std::optional<std::pair<Point3, double>> faceInteriorPoint(const TopoDS_Face& F) {
        double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
        BRepTools::UVBounds(F, u0, u1, v0, v1);
        BRepTopAdaptor_FClass2d cls(F, 1e-9);
        BRepAdaptor_Surface surf(F, false);
        std::optional<std::pair<Point3, double>> best;
        constexpr int N = 5;
        for (int i = 1; i < N; ++i)
            for (int j = 1; j < N; ++j) {
                const double u = u0 + (u1 - u0) * i / N, v = v0 + (v1 - v0) * j / N;
                if (cls.Perform(gp_Pnt2d(u, v)) != TopAbs_IN) continue;
                const gp_Pnt P = surf.Value(u, v);
                double clear = 1e300;
                const TopoDS_Vertex V = BRepBuilderAPI_MakeVertex(P).Vertex();
                for (TopExp_Explorer ex(F, TopAbs_EDGE); ex.More(); ex.Next()) {
                    BRepExtrema_DistShapeShape ds(V, ex.Current());
                    if (ds.IsDone()) clear = std::min(clear, ds.Value());
                }
                if (!best || clear > best->second) best = std::pair<Point3, double>{pt(P), clear};
            }
        return best;
    }

    // Fallback inward shell by "erosion" (§3.13, §9.6б): the wall is body points closer than t to the kept boundary.
    // Tools: a layer per kept face (planar: the face prism of depth t inward; otherwise: the face and its offset at
    // t inward), a tube of radius t along each concave edge between kept faces, a ball of radius t at each
    // vertex of such an edge. Cavity: the body with the tools subtracted one by one; result: the body minus the cavity.
    // History is geometric, by an interior point of each result face: on a body face — FromFace, on the layer offset of
    // face g — Offset{0, g}, on a tube — FromEdge{logical edge}, on a ball — FromVertex;
    // rim faces — as in the main method.
    Result<OpResult> shellByErosion(const Shape& body, const std::vector<int>& removeFaces, double t, const Cancel* cancel) const {
        const OcctShape& d = D(body);
        auto removed = [&](int f) { return std::find(removeFaces.begin(), removeFaces.end(), f) != removeFaces.end(); };
        const CanonicalTopology ct = canonicalTopology(*this, body);
        std::map<int, int> logicalOf;  // kernel edge → logical edge
        for (size_t le = 0; le < ct.logicalEdges.size(); ++le)
            for (int ke : ct.logicalEdges[le].edges) logicalOf[ke] = static_cast<int>(le);
        try {
            std::vector<TopoDS_Shape> tools;
            std::vector<std::pair<TopoDS_Face, Origin>> toolFaces;  // tool face → origin
            // 1. Layers of the kept faces.
            for (int f = 0; f < d.faces.Extent(); ++f) {
                if (removed(f)) continue;
                if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
                const TopoDS_Face& F = TopoDS::Face(d.faces(f + 1));
                TopoDS_Shape slab;
                if (BRepAdaptor_Surface(F, false).GetType() == GeomAbs_Plane) {
                    const Vec3 n = faceNormalAt(body, f, faceCentroid(body, f));
                    TopoDS_Face base = F;
                    base.Orientation(TopAbs_FORWARD);
                    BRepPrimAPI_MakePrism prism(base, gv(-t * n), true);
                    if (!prism.IsDone()) return Error("KERNEL_SHELL_FAILED");
                    slab = prism.Shape();
                } else {
                    BRepOffsetAPI_MakeThickSolid mk;
                    mk.MakeThickSolidBySimple(F, -t);
                    mk.Build();
                    if (!mk.IsDone()) return Error("KERNEL_SHELL_FAILED");
                    const auto ss = solidsOf(mk.Shape());
                    if (ss.size() != 1) return Error("KERNEL_SHELL_FAILED");
                    slab = ss.front();
                }
                GProp_GProps g;
                BRepGProp::VolumeProperties(slab, g);
                if (!(std::abs(g.Mass()) > 0)) return Error("KERNEL_SHELL_FAILED");
                if (g.Mass() < 0) slab.Reverse();
                tools.push_back(slab);
                for (TopExp_Explorer ex(slab, TopAbs_FACE); ex.More(); ex.Next()) {
                    const TopoDS_Face& sf = TopoDS::Face(ex.Current());
                    GProp_GProps sg;
                    BRepGProp::SurfaceProperties(sf, sg);
                    const double dist = distanceToFace(body, f, pt(sg.CentreOfMass()));
                    // The face offset gives Offset; the face itself (a body face in the result) and the layer side faces (on
                    // the result boundary only at the opening, where a rim face replaces them) give no origin.
                    if (dist <= 0.75 * t) continue;
                    Origin o;
                    o.kind = Origin::Kind::Offset;
                    o.input = {0, f};
                    toolFaces.push_back({sf, o});
                }
            }
            // 2. Tubes along concave edges between kept faces and balls at their vertices.
            std::set<int> corners;
            for (int ke = 0; ke < d.edges.Extent(); ++ke) {
                const auto& ef = d.edgeFaces[size_t(ke)];
                if (ef.size() != 2 || ef[0] == ef[1] || removed(ef[0]) || removed(ef[1]) || !logicalOf.count(ke)) continue;
                const TopoDS_Edge& E = TopoDS::Edge(d.edges(ke + 1));
                BRepAdaptor_Curve c(E);
                const double um = 0.5 * (c.FirstParameter() + c.LastParameter());
                gp_Pnt P;
                gp_Vec T;
                c.D1(um, P, T);
                if (T.Magnitude() < 1e-12) continue;
                T.Normalize();
                const Vec3 nA = faceNormalAt(body, ef[0], pt(P)), nB = faceNormalAt(body, ef[1], pt(P));
                Vec3 dA = nA.cross(Vec3(T.X(), T.Y(), T.Z()));  // in the plane of face A, across the edge
                if (dA.norm() < 1e-12) continue;
                dA.normalize();
                const double eps = 1e-3 * std::max(1.0, t);
                if (distanceToFace(body, ef[0], pt(P) + eps * dA) > distanceToFace(body, ef[0], pt(P) - eps * dA)) dA = -dA;
                if (nB.dot(dA) <= KERNEL_SHELL_CONCAVE_TOL) continue;  // convex or smooth: covered by the layers
                TopoDS_Shape tube;
                if (c.GetType() == GeomAbs_Line) {
                    const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
                    const double len = a.Distance(b);
                    if (len < KERNEL_MIN_DIMENSION) continue;
                    tube = BRepPrimAPI_MakeCylinder(gp_Ax2(a, gp_Dir(gp_Vec(a, b))), t, len).Shape();
                } else if (c.GetType() == GeomAbs_Circle) {
                    const gp_Circ circ = c.Circle();
                    const double u0 = c.FirstParameter(), u1 = c.LastParameter();
                    gp_Ax2 ax = circ.Position();
                    ax.Rotate(ax.Axis(), u0);
                    const double ang = u1 - u0;
                    tube = ang >= 2 * PI - 1e-9 ? BRepPrimAPI_MakeTorus(ax, circ.Radius(), t).Shape()
                                                : BRepPrimAPI_MakeTorus(ax, circ.Radius(), t, ang).Shape();
                } else {
                    const gp_Circ prof(gp_Ax2(c.Value(c.FirstParameter()), gp_Dir(c.DN(c.FirstParameter(), 1))), t);
                    const TopoDS_Face pf = BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(prof).Edge()).Wire()).Face();
                    BRepOffsetAPI_MakePipe pipe(BRepBuilderAPI_MakeWire(E).Wire(), pf);
                    pipe.Build();
                    if (!pipe.IsDone()) return Error("KERNEL_SHELL_FAILED");
                    tube = pipe.Shape();
                }
                tools.push_back(tube);
                Origin o;
                o.kind = Origin::Kind::FromEdge;
                o.edges = {logicalOf[ke]};
                for (TopExp_Explorer ex(tube, TopAbs_FACE); ex.More(); ex.Next()) toolFaces.push_back({TopoDS::Face(ex.Current()), o});
                corners.insert(d.edgeVerts[size_t(ke)].first);
                corners.insert(d.edgeVerts[size_t(ke)].second);
            }
            for (int v : corners) {
                if (v < 0) continue;
                const TopoDS_Shape ball = BRepPrimAPI_MakeSphere(BRep_Tool::Pnt(TopoDS::Vertex(d.vertices(v + 1))), t).Shape();
                tools.push_back(ball);
                Origin o;
                o.kind = Origin::Kind::FromVertex;
                o.vertex = v;
                for (TopExp_Explorer ex(ball, TopAbs_FACE); ex.More(); ex.Next()) toolFaces.push_back({TopoDS::Face(ex.Current()), o});
            }
            // 3. Cavity: tools are subtracted from the body one by one (a single operation with all tools on touching and
            // coincident layer faces gives an invalid result); the wall is the body minus the cavity.
            TopoDS_Shape cavity = d.solids.front();
            for (const TopoDS_Shape& tool : tools) {
                if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
                BRepAlgoAPI_Cut cut(cavity, tool);
                if (!cut.IsDone()) return Error("KERNEL_SHELL_FAILED");
                cavity = cut.Shape();
            }
            BRepAlgoAPI_Cut wallOp(d.solids.front(), cavity);
            if (!wallOp.IsDone()) return Error("KERNEL_SHELL_FAILED");
            const auto solids = solidsOf(wallOp.Shape());
            if (solids.size() != 1) return Error("KERNEL_SHELL_FAILED");
            auto sd = makeShapeData(solids);
            OpResult r;
            r.shape = wrap(sd);
            const double v0 = massProperties(body).volume, v1 = massProperties(r.shape).volume;
            if (!(v1 > 0) || !(v1 < v0) || !checkValidity(r.shape).valid) return Error("KERNEL_SHELL_FAILED");
            // 4. History from geometry: an interior point of a result face lies on a body face or a tool face.
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            std::vector<bool> bodyFaceUsed(size_t(d.faces.Extent()), false);
            auto onFace = [](const TopoDS_Shape& face, const gp_Pnt& q) {
                BRepExtrema_DistShapeShape ds(BRepBuilderAPI_MakeVertex(q).Vertex(), face);
                return ds.IsDone() && ds.Value() <= KERNEL_SHELL_MATCH_TOL;
            };
            for (int fi = 0; fi < sd->faces.Extent(); ++fi) {
                const auto ip = faceInteriorPoint(TopoDS::Face(sd->faces(fi + 1)));
                const gp_Pnt q = gp(ip ? ip->first : faceCentroid(r.shape, fi));
                const Vec3 nr = faceNormalAt(r.shape, fi, pt(q));
                auto& origins = r.history.faceOrigins[size_t(fi)];
                for (int f = 0; f < d.faces.Extent() && origins.empty(); ++f) {
                    if (removed(f) || !onFace(d.faces(f + 1), q) || faceNormalAt(body, f, pt(q)).dot(nr) < 1 - 1e-6) continue;
                    Origin o;
                    o.kind = Origin::Kind::FromFace;
                    o.input = {0, f};
                    o.modified = !sd->faces(fi + 1).IsSame(d.faces(f + 1));
                    origins.push_back(o);
                    bodyFaceUsed[size_t(f)] = true;
                }
                for (const auto& [tf, o] : toolFaces) {
                    if (!origins.empty()) break;
                    if (onFace(tf, q)) origins.push_back(o);
                }
            }
            for (int f = 0; f < d.faces.Extent(); ++f)
                if (!bodyFaceUsed[size_t(f)]) r.history.deletedFaces.push_back({0, f});
            assignRimFaces(body, removeFaces, r);
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_SHELL_FAILED");
        }
    }

    // Opening rim faces without history are the remainder of a removed face on its surface (§3.13): FromFace{removed,
    // MODIFIED}; a removed face with a remainder is not considered removed.
    void assignRimFaces(const Shape& body, const std::vector<int>& removeFaces, OpResult& r) const {
        const OcctShape& d = D(body);
        for (size_t fi = 0; fi < r.history.faceOrigins.size(); ++fi) {
            if (!r.history.faceOrigins[fi].empty()) continue;
            const Point3 c = faceCentroid(r.shape, static_cast<int>(fi));
            for (int f : removeFaces) {
                GeomAPI_ProjectPointOnSurf proj(gp(c), BRep_Tool::Surface(TopoDS::Face(d.faces(f + 1))));
                if (proj.NbPoints() == 0 || proj.LowerDistance() > KERNEL_SHELL_RIM_TOL) continue;
                if (faceNormalAt(r.shape, static_cast<int>(fi), c).dot(faceNormalAt(body, f, c)) < 1 - 1e-9) continue;
                Origin o;
                o.kind = Origin::Kind::FromFace;
                o.input = {0, f};
                o.modified = true;
                r.history.faceOrigins[fi].push_back(o);
                auto& del = r.history.deletedFaces;
                del.erase(std::remove(del.begin(), del.end(), InputFace{0, f}), del.end());
                break;
            }
        }
    }

    // Main method (§9.6б): MakeThickSolidByJoin.
    Result<OpResult> shellByJoin(const Shape& body, const std::vector<int>& removeFaces, double thickness, bool outward,
                                 const Cancel* cancel) const {
        if (body.null() || D(body).solids.size() != 1) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("operand"));
        const OcctShape& d = D(body);
        for (int f : removeFaces)
            if (f < 0 || f >= d.faces.Extent())
                return Error("KERNEL_INVALID_INDEX").with("name", std::string("face")).with("actual", static_cast<double>(f));
        if (!(thickness >= KERNEL_MIN_DIMENSION && thickness <= KERNEL_MAX_COORD))
            return Error("KERNEL_INVALID_DIMENSION").with("name", std::string("thickness")).with("actual", thickness).with("expected", KERNEL_MIN_DIMENSION);
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        try {
            NCollection_List<TopoDS_Shape> closing;
            for (int f : removeFaces) closing.Append(d.faces(f + 1));
            BRepOffsetAPI_MakeThickSolid mk;
            mk.MakeThickSolidByJoin(d.solids.front(), closing, outward ? thickness : -thickness, 1e-6, BRepOffset_Skin, false, false,
                                    GeomAbs_Intersection);
            mk.Build();
            if (!mk.IsDone()) return Error("KERNEL_SHELL_FAILED");
            const auto solids = solidsOf(mk.Shape());
            if (solids.size() != 1) return Error("KERNEL_SHELL_FAILED");
            auto sd = makeShapeData(solids);
            // OCCT sometimes "builds" an impossible wall (inward, thicker than the body): the result must be valid and,
            // for an inward wall, smaller in volume than the original body (§3.13).
            const double v0 = massProperties(body).volume, v1 = massProperties(wrap(sd)).volume;
            if (!(v1 > 0) || (!outward && !(v1 < v0)) || !checkValidity(wrap(sd)).valid)
                return Error("KERNEL_SHELL_FAILED");
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            for (int f = 1; f <= d.faces.Extent(); ++f) {
                const TopoDS_Shape& F = d.faces(f);
                bool used = false;
                const bool removed = std::find(removeFaces.begin(), removeFaces.end(), f - 1) != removeFaces.end();
                if (!removed && !mk.IsDeleted(F)) {
                    for (const TopoDS_Shape& m : mk.Modified(F)) {
                        const int fi = sd->faces.FindIndex(m);
                        if (fi <= 0) continue;
                        Origin o;
                        o.kind = Origin::Kind::FromFace;
                        o.input = {0, f - 1};
                        o.modified = true;
                        r.history.faceOrigins[fi - 1].push_back(o);
                        used = true;
                    }
                    if (!used) {
                        const int fi = sd->faces.FindIndex(F);
                        if (fi > 0) {
                            Origin o;
                            o.kind = Origin::Kind::FromFace;
                            o.input = {0, f - 1};
                            r.history.faceOrigins[fi - 1].push_back(o);
                            used = true;
                        }
                    }
                }
                if (!used) r.history.deletedFaces.push_back({0, f - 1});
                for (const TopoDS_Shape& g : mk.Generated(F)) {
                    const int fi = sd->faces.FindIndex(g);
                    if (fi <= 0) continue;
                    Origin o;
                    o.kind = Origin::Kind::Offset;
                    o.input = {0, f - 1};
                    r.history.faceOrigins[fi - 1].push_back(o);
                }
            }
            const CanonicalTopology ct = canonicalTopology(*this, body);
            std::map<int, std::set<int>> fromEdges;
            for (size_t le = 0; le < ct.logicalEdges.size(); ++le)
                for (int ke : ct.logicalEdges[le].edges)
                    for (const TopoDS_Shape& g : mk.Generated(d.edges(ke + 1))) {
                        const int fi = sd->faces.FindIndex(g);
                        if (fi > 0) fromEdges[fi - 1].insert(static_cast<int>(le));
                    }
            for (auto& [fi, les] : fromEdges) {
                Origin o;
                o.kind = Origin::Kind::FromEdge;
                o.edges.assign(les.begin(), les.end());
                r.history.faceOrigins[fi].push_back(o);
            }
            r.shape = wrap(sd);
            // OCCT returns rim faces without history (§3.13).
            assignRimFaces(body, removeFaces, r);
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_SHELL_FAILED");
        }
    }

    Result<OpResult> draft(const Shape& body, const std::vector<int>& faces, const Point3& neutralPoint, const Vec3& pull, double angle,
                           const Cancel* cancel) const override {
        if (body.null() || D(body).solids.size() != 1) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("operand"));
        if (faces.empty()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("faces"));
        const OcctShape& d = D(body);
        for (int f : faces)
            if (f < 0 || f >= d.faces.Extent())
                return Error("KERNEL_INVALID_INDEX").with("name", std::string("face")).with("actual", static_cast<double>(f));
        if (!pull.allFinite() || std::abs(pull.norm() - 1) > 1e-9) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("axis"));
        if (!(angle >= KERNEL_MIN_ANGLE && angle <= PI / 2 - KERNEL_MIN_ANGLE))
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("angle"));
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        try {
            BRepOffsetAPI_DraftAngle mk(d.solids.front());
            const gp_Dir dir(pull.x(), pull.y(), pull.z());
            const gp_Pln neutral(gp(neutralPoint), dir);
            for (int f : faces) {
                mk.Add(TopoDS::Face(d.faces(f + 1)), dir, angle, neutral);
                if (!mk.AddDone()) return Error("KERNEL_DRAFT_FAILED");
            }
            mk.Build();
            if (!mk.IsDone()) return Error("KERNEL_DRAFT_FAILED");
            const auto solids = solidsOf(mk.Shape());
            if (solids.size() != 1) return Error("KERNEL_DRAFT_FAILED");
            auto sd = makeShapeData(solids);
            if (!(massProperties(wrap(sd)).volume > 0) || !checkValidity(wrap(sd)).valid) return Error("KERNEL_DRAFT_FAILED");
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            for (int f = 1; f <= d.faces.Extent(); ++f) {
                const TopoDS_Shape& F = d.faces(f);
                bool used = false;
                for (const TopoDS_Shape& m : mk.Modified(F)) {
                    const int fi = sd->faces.FindIndex(m);
                    if (fi <= 0) continue;
                    Origin o;
                    o.kind = Origin::Kind::FromFace;
                    o.input = {0, f - 1};
                    o.modified = true;
                    r.history.faceOrigins[fi - 1].push_back(o);
                    used = true;
                }
                if (!used) {
                    const int fi = sd->faces.FindIndex(mk.ModifiedShape(F));
                    if (fi > 0) {
                        Origin o;
                        o.kind = Origin::Kind::FromFace;
                        o.input = {0, f - 1};
                        o.modified = !mk.ModifiedShape(F).IsSame(F);
                        r.history.faceOrigins[fi - 1].push_back(o);
                        used = true;
                    }
                }
                if (!used) r.history.deletedFaces.push_back({0, f - 1});
            }
            r.shape = wrap(sd);
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_DRAFT_FAILED");
        }
    }

    // kernel-interface.md §3.19, §9.6д.
    Result<Shape> unifyFaces(const Shape& s) const override {
        const auto solids = D(s).solids;
        if (solids.size() != 1) return s;
        try {
            ShapeUpgrade_UnifySameDomain unify(solids.front(), true, true, false);
            unify.Build();
            TopoDS_Shape merged = unify.Shape();
            // Pieces of one periodic surface whose pcurves lie in different periods (a boolean with a solid of revolution
            // ending at the surface's seam) are left unmerged by OCCT 8: then the pcurves are fixed and the merge repeated.
            if (sharesSurfaceAcrossEdge(merged)) {
                ShapeFix_Shape fix(merged);
                fix.Perform();
                ShapeUpgrade_UnifySameDomain again(fix.Shape(), true, true, false);
                again.Build();
                TopTools_IndexedMapOfShape f0, f1;
                TopExp::MapShapes(merged, TopAbs_FACE, f0);
                TopExp::MapShapes(again.Shape(), TopAbs_FACE, f1);
                if (f1.Extent() < f0.Extent() && solidsOf(again.Shape()).size() == 1) merged = again.Shape();
            }
            const auto out = solidsOf(merged);
            if (out.size() != 1) return s;
            Shape r = wrap(makeShapeData(out));
            return checkValidity(r).valid ? r : s;
        } catch (const Standard_Failure&) {
            return s;
        }
    }

    Result<OpResult> removeFaces(const Shape& body, const std::vector<int>& faces, const Cancel* cancel) const override {
        if (body.null() || D(body).solids.size() != 1) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("operand"));
        if (faces.empty()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("faces"));
        const OcctShape& d = D(body);
        for (int f : faces)
            if (f < 0 || f >= d.faces.Extent())
                return Error("KERNEL_INVALID_INDEX").with("name", std::string("face")).with("actual", static_cast<double>(f));
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        try {
            BRepAlgoAPI_Defeaturing mk;
            mk.SetShape(d.solids.front());
            for (int f : faces) mk.AddFaceToRemove(d.faces(f + 1));
            mk.SetRunParallel(false);
            mk.SetToFillHistory(true);
            CancelIndicator progress(cancel);
            mk.Build(progress.Start());
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            if (!mk.IsDone() || mk.HasErrors()) return Error("KERNEL_REMOVE_FACES_FAILED");
            const auto solids = solidsOf(mk.Shape());
            if (solids.size() != 1) return Error("KERNEL_REMOVE_FACES_FAILED");
            auto sd = makeShapeData(solids);
            if (!(massProperties(wrap(sd)).volume > 0) || !checkValidity(wrap(sd)).valid) return Error("KERNEL_REMOVE_FACES_FAILED");
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            std::set<int> removed(faces.begin(), faces.end());
            for (int f = 1; f <= d.faces.Extent(); ++f) {
                const TopoDS_Shape& F = d.faces(f);
                bool used = false;
                if (!removed.count(f - 1) && !mk.IsDeleted(F)) {
                    const auto& mods = mk.Modified(F);
                    if (mods.IsEmpty()) {
                        const int fi = sd->faces.FindIndex(F);
                        if (fi > 0) {
                            Origin o;
                            o.kind = Origin::Kind::FromFace;
                            o.input = {0, f - 1};
                            r.history.faceOrigins[fi - 1].push_back(o);
                            used = true;
                        }
                    }
                    for (const TopoDS_Shape& m : mods) {
                        const int fi = sd->faces.FindIndex(m);
                        if (fi <= 0) continue;
                        Origin o;
                        o.kind = Origin::Kind::FromFace;
                        o.input = {0, f - 1};
                        o.modified = true;
                        r.history.faceOrigins[fi - 1].push_back(o);
                        used = true;
                    }
                }
                if (!used) r.history.deletedFaces.push_back({0, f - 1});
            }
            for (auto& v : r.history.faceOrigins) sortOrigins(v);
            r.shape = wrap(sd);
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_REMOVE_FACES_FAILED");
        }
    }

    // kernel-interface.md §3.20, §9.6е.
    // Section curve of a non-analytic kind as a line, arc or circle (§9.6е): KERNEL_SECTION_FIT_SAMPLES + 1 points evenly
    // by parameter; a line through the ends, otherwise the circle with the center through the ends and the middle
    // (closed: samples 0, n/3, 2n/3) and the mean radius; every sample within KERNEL_SECTION_FIT_TOL of it; else nullopt.
    template <class UV>
    static std::optional<Curve2> fitSectionCurve(const BRepAdaptor_Curve& c, UV uv) {
        const double t0 = c.FirstParameter(), t1 = c.LastParameter();
        std::vector<Point2> q;
        for (int i = 0; i <= KERNEL_SECTION_FIT_SAMPLES; ++i) q.push_back(uv(c.Value(t0 + (t1 - t0) * i / KERNEL_SECTION_FIT_SAMPLES)));
        const Point2 a = q.front(), b = q.back(), m = q[q.size() / 2];
        const bool closed = (a - b).norm() <= KERNEL_SECTION_FIT_TOL;
        Curve2 k;
        if (!closed && (b - a).norm() > KERNEL_SECTION_FIT_TOL) {
            const Vec2 u = (b - a).normalized();
            double dev = 0;
            for (const auto& p : q) dev = std::max(dev, std::abs(u.x() * (p - a).y() - u.y() * (p - a).x()));
            if (dev <= KERNEL_SECTION_FIT_TOL) {
                k.kind = Curve2::Kind::Line;
                k.p1 = a, k.p2 = b;
                return k;
            }
        }
        // Circle through three points.
        const Point2 p1 = a, p2 = closed ? q[q.size() / 3] : m, p3 = closed ? q[2 * q.size() / 3] : b;
        const double d = 2 * (p1.x() * (p2.y() - p3.y()) + p2.x() * (p3.y() - p1.y()) + p3.x() * (p1.y() - p2.y()));
        if (std::abs(d) < 1e-12) return std::nullopt;
        const double s1 = p1.squaredNorm(), s2 = p2.squaredNorm(), s3 = p3.squaredNorm();
        const Point2 ctr((s1 * (p2.y() - p3.y()) + s2 * (p3.y() - p1.y()) + s3 * (p1.y() - p2.y())) / d,
                         (s1 * (p3.x() - p2.x()) + s2 * (p1.x() - p3.x()) + s3 * (p2.x() - p1.x())) / d);
        // Radius: mean distance of the samples to the center (less sensitive to the spline error than three points).
        double r = 0;
        for (const auto& p : q) r += (p - ctr).norm();
        r /= double(q.size());
        for (const auto& p : q)
            if (std::abs((p - ctr).norm() - r) > KERNEL_SECTION_FIT_TOL) return std::nullopt;
        k.center = ctr;
        k.radius = r;
        if (closed) {
            k.kind = Curve2::Kind::Circle;
            return k;
        }
        // Counterclockwise about the plane normal: from the end whose sweep to the other passes the middle.
        auto ang = [&](const Point2& p) { return std::atan2(p.y() - ctr.y(), p.x() - ctr.x()); };
        auto sweep = [](double from, double to) { return geom2::ccwSweep(from, to); };
        k.kind = Curve2::Kind::Arc;
        k.start = a, k.end = b;
        if (sweep(ang(a), ang(m)) > sweep(ang(a), ang(b))) std::swap(k.start, k.end);
        return k;
    }

    // §3.20 (FX7-SH-004): a B-spline or Bezier section edge as a NURBS in the plane coordinates — the poles of the piece
    // (trimmed to the edge range) projected; exact for a curve lying in the plane.
    template <class UV>
    static std::optional<Curve2> sectionSpline(const TopoDS_Edge& e, UV uv) {
        double f = 0, l = 0;
        const Handle(Geom_Curve) gc = BRep_Tool::Curve(e, f, l);
        if (gc.IsNull() || !(l > f)) return std::nullopt;
        Handle(Geom_BSplineCurve) b;
        try {
            b = GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(gc, f, l));
            if (!b.IsNull() && b->IsPeriodic()) b->SetNotPeriodic();
        } catch (const Standard_Failure&) {
            return std::nullopt;
        }
        if (b.IsNull()) return std::nullopt;
        spline::Curve c;
        c.degree = b->Degree();
        for (int i = 1; i <= b->NbPoles(); ++i) c.poles.push_back(uv(b->Pole(i)));
        if (b->IsRational())
            for (int i = 1; i <= b->NbPoles(); ++i) c.weights.push_back(b->Weight(i));
        for (int i = 1; i <= b->NbKnots(); ++i) {
            c.knots.push_back(b->Knot(i));
            c.mults.push_back(b->Multiplicity(i));
        }
        if (!spline::validate(c).empty()) return std::nullopt;
        Curve2 k;
        k.kind = Curve2::Kind::Spline;
        k.p1 = c.poles.front();
        k.p2 = c.poles.back();
        k.spline = std::move(c);
        return k;
    }

    Result<std::vector<Curve2>> planeSection(const Shape& s, const Plane& pl, bool splines) const override {
        std::vector<Curve2> out;
        if (s.null() || D(s).shape.IsNull()) return out;
        auto uv = [&](const gp_Pnt& p) { const Point3 q = pt(p); return Point2((q - pl.origin).dot(pl.x), (q - pl.origin).dot(pl.y)); };
        try {
            BRepAlgoAPI_Section sec(D(s).shape, gp_Pln(gp(pl.origin), gd(pl.normal)), false);
            sec.Approximation(false);
            sec.ComputePCurveOn1(false);
            sec.ComputePCurveOn2(false);
            sec.Build();
            if (!sec.IsDone()) return Error("KERNEL_SECTION_FAILED").with("detail", std::string("not done"));
            for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
                const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
                if (BRep_Tool::Degenerated(e)) continue;
                BRepAdaptor_Curve c(e);
                TopoDS_Vertex v1, v2;
                TopExp::Vertices(e, v1, v2, true);
                const gp_Pnt a = BRep_Tool::Pnt(v1), b = BRep_Tool::Pnt(v2);
                Curve2 k;
                if (c.GetType() == GeomAbs_Line) {
                    if (a.Distance(b) <= LINEAR_TOL) continue;
                    k.kind = Curve2::Kind::Line;
                    k.p1 = uv(a);
                    k.p2 = uv(b);
                } else if (c.GetType() == GeomAbs_Circle) {
                    const gp_Circ ci = c.Circle();
                    k.center = uv(ci.Location());
                    k.radius = ci.Radius();
                    const double span = std::abs(c.LastParameter() - c.FirstParameter());
                    if (a.Distance(b) <= LINEAR_TOL && span > PI) {
                        k.kind = Curve2::Kind::Circle;
                    } else {
                        Point2 p1 = uv(a), p2 = uv(b);
                        const Point2 m = uv(c.Value((c.FirstParameter() + c.LastParameter()) / 2));
                        auto ang = [&](const Point2& q) { return std::atan2(q.y() - k.center.y(), q.x() - k.center.x()); };
                        auto sweep = [](double from, double to) { return geom2::ccwSweep(from, to); };
                        if (sweep(ang(p1), ang(m)) > sweep(ang(p1), ang(p2))) std::swap(p1, p2);
                        k.kind = Curve2::Kind::Arc;
                        k.start = p1;
                        k.end = p2;
                    }
                } else {
                    // A curve of another kind (an intersection spline: a section of a torus through its axis) that lies
                    // on a line or a circle within KERNEL_SECTION_FIT_TOL is that line or arc (§9.6е).
                    auto fit = fitSectionCurve(c, uv);
                    if (!fit && splines && (c.GetType() == GeomAbs_BSplineCurve || c.GetType() == GeomAbs_BezierCurve))
                        fit = sectionSpline(e, uv);
                    if (!fit) return Error("KERNEL_NOT_SUPPORTED").with("name", std::string("curveType")).with("actual", double(c.GetType()));
                    k = *fit;
                }
                out.push_back(k);
            }
        } catch (const Standard_Failure&) {
            return Error("KERNEL_SECTION_FAILED").with("detail", std::string("exception"));
        }
        return out;
    }

    Result<OpResult> loft(const std::vector<Profile>& sectionsIn, const Cancel* cancel) const override {
        if (sectionsIn.size() < 2) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("sections"));
        std::vector<Profile> sections;
        for (const auto& s : sectionsIn) {
            Profile p;
            auto info = validateProfile(s, &p);
            if (!info) return info.error();
            if (!p.holes.empty()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("sections"));
            sections.push_back(p);
        }
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        try {
            BRepOffsetAPI_ThruSections ts(true, false, 1e-6);
            std::vector<std::pair<TopoDS_Shape, SegmentId>> firstEdges;
            std::vector<TopoDS_Wire> wires;
            bool starts = false;
            for (size_t i = 0; i < sections.size(); ++i) {
                auto bf = buildFace(sections[i]);
                if (!bf) return Error("KERNEL_LOFT_FAILED");
                TopoDS_Wire w = BRepTools::OuterWire(bf->face);
                // Section traversal is aligned with the direction of the first one: normals point the same way (§3.15).
                if (i > 0 && sections[i].plane.normal.dot(sections[0].plane.normal) < 0) w.Reverse();
                if (i == 0) firstEdges = bf->edgeIds;
                // A given start (FX10-PT-004): the wire is rebuilt from the edge whose start vertex is nearest to it.
                if (const auto& st = sectionsIn[i].start) {
                    starts = true;
                    std::vector<TopoDS_Edge> edges;
                    size_t first = 0;
                    double best = std::numeric_limits<double>::infinity();
                    for (BRepTools_WireExplorer ex(w); ex.More(); ex.Next()) {
                        const gp_Pnt q = BRep_Tool::Pnt(ex.CurrentVertex());
                        const double dist = (Point3(q.X(), q.Y(), q.Z()) - *st).norm();
                        if (dist < best) best = dist, first = edges.size();
                        edges.push_back(ex.Current());
                    }
                    BRepBuilderAPI_MakeWire mw;
                    for (size_t k = 0; k < edges.size(); ++k) mw.Add(edges[(first + k) % edges.size()]);
                    if (!mw.IsDone()) return Error("KERNEL_LOFT_FAILED");
                    w = mw.Wire();
                }
                wires.push_back(w);
            }
            // The automatic choice of the starting vertices (compatible wires) would override given starts; it is kept
            // only when no start is given or the sections differ in the number of edges (§3.15).
            bool sameCount = true;
            auto edgeCount = [](const TopoDS_Wire& w) {
                int n = 0;
                for (BRepTools_WireExplorer ex(w); ex.More(); ex.Next()) ++n;
                return n;
            };
            for (const auto& w : wires) sameCount = sameCount && edgeCount(w) == edgeCount(wires.front());
            ts.CheckCompatibility(!(starts && sameCount));
            for (const auto& w : wires) ts.AddWire(w);
            ts.Build();
            if (!ts.IsDone()) return Error("KERNEL_LOFT_FAILED");
            const auto solids = solidsOf(ts.Shape());
            if (solids.size() != 1) return Error("KERNEL_LOFT_FAILED");
            auto sd = makeShapeData(solids);
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            tagProfileFaces(ts, *sd, firstEdges, r.history);
            for (auto& o : r.history.faceOrigins) sortOrigins(o);
            r.shape = wrap(sd);
            if (!(massProperties(r.shape).volume > 0) || !checkValidity(r.shape).valid) return Error("KERNEL_LOFT_FAILED");
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_LOFT_FAILED");
        }
    }

    Result<OpResult> helicalSweep(const Profile& pIn, const Axis& axis, double pitch, double turns, const Cancel* cancel) const override {
        // §3.25, §9.6и
        Profile p;
        auto info = validateProfile(pIn, &p);
        if (!info) return info.error();
        if (!p.holes.empty()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("profile"));
        auto finite = [](const Vec3& v) { return v.allFinite() && v.cwiseAbs().maxCoeff() <= KERNEL_MAX_COORD; };
        if (!finite(axis.origin) || !finite(axis.direction) || std::abs(axis.direction.norm() - 1) > 1e-9)
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("axis"));
        const Vec3& n = p.plane.normal;
        const Vec3 a = axis.direction;
        const double dz = std::abs(a.dot(n));
        const double dist = std::abs((axis.origin - p.plane.origin).dot(n));
        if (dz > KERNEL_AXIS_ANGLE_TOL || dist > KERNEL_PROFILE_GAP_TOL)
            return Error("KERNEL_AXIS_NOT_IN_PLANE").with("angle", std::asin(std::min(1.0, dz))).with("distance", dist);
        if (!(std::isfinite(pitch) && pitch > 0))
            return Error("KERNEL_INVALID_DIMENSION").with("name", std::string("pitch")).with("actual", pitch).with("expected", 0.0);
        if (!(std::isfinite(turns) && turns > 0 && turns <= KERNEL_HELIX_MAX_TURNS))
            return Error("KERNEL_INVALID_DIMENSION").with("name", std::string("turns")).with("actual", turns)
                .with("expected", turns > 0 ? KERNEL_HELIX_MAX_TURNS : 0.0);
        if (pitch * turns > KERNEL_MAX_COORD)
            return Error("KERNEL_INVALID_DIMENSION").with("name", std::string("pitch")).with("actual", pitch).with("expected", KERNEL_MAX_COORD / turns);
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        try {
            auto bf = buildFace(p);
            if (!bf) return Error("KERNEL_HELIX_FAILED");
            // Side of the axis and axial extent by edge samples.
            const Vec3 w = n.cross(a).normalized();  // in-plane, perpendicular to the axis
            int side = 0;
            double lo = 1e300, hi = -1e300;
            for (const auto& [e, id] : bf->edgeIds) {
                BRepAdaptor_Curve c(TopoDS::Edge(e));
                for (int k = 0; k <= 32; ++k) {
                    const Point3 q = pt(c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / 32));
                    const double sd = (q - axis.origin).dot(w);
                    const int sg = sd > 0 ? 1 : -1;
                    if (std::abs(sd) <= KERNEL_MIN_DIMENSION || (side != 0 && sg != side))
                        return Error("KERNEL_PROFILE_CROSSES_AXIS").with("id", id);
                    side = sg;
                    const double t = (q - axis.origin).dot(a);
                    lo = std::min(lo, t), hi = std::max(hi, t);
                }
            }
            if (pitch <= hi - lo)
                return Error("KERNEL_INVALID_DIMENSION").with("name", std::string("pitch")).with("actual", pitch).with("expected", hi - lo);
            const Vec3 u = double(side) * w;  // from the axis towards the profile
            GProp_GProps props;
            BRepGProp::SurfaceProperties(bf->face, props);
            const Point3 g = pt(props.CentreOfMass());
            const double area = props.Mass();
            const double rho = (g - axis.origin).dot(u);
            const Point3 base = axis.origin + (g - axis.origin).dot(a) * a;
            // Spine: helix of radius rho through the profile centroid, one edge per (at most) one turn.
            Handle(Geom_CylindricalSurface) cyl = new Geom_CylindricalSurface(gp_Ax3(gp(base), gd(a), gd(u)), rho);
            const double turnLen = std::hypot(2 * PI, pitch);
            Handle(Geom2d_Line) line = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(2 * PI, pitch));
            BRepBuilderAPI_MakeWire mw;
            for (int i = 0; i < int(std::ceil(turns - 1e-9)); ++i) {
                const double t0 = turnLen * i, t1 = turnLen * std::min<double>(i + 1, turns);
                BRepBuilderAPI_MakeEdge me(line, cyl, t0, t1);
                if (!me.IsDone()) return Error("KERNEL_HELIX_FAILED");
                TopoDS_Edge e = me.Edge();
                BRepLib::BuildCurves3d(e, 1e-7, GeomAbs_C2, 14, 200);
                mw.Add(e);
                if (!mw.IsDone()) return Error("KERNEL_HELIX_FAILED");
            }
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            BRepOffsetAPI_MakePipeShell ps(mw.Wire());
            ps.SetMode(true);  // Frenet: exact screw motion along a helix
            ps.Add(BRepTools::OuterWire(bf->face), false, false);
            ps.Build();
            if (!ps.IsDone() || !ps.MakeSolid()) return Error("KERNEL_HELIX_FAILED");
            const auto solids = solidsOf(ps.Shape());
            if (solids.size() != 1) return Error("KERNEL_HELIX_FAILED");
            auto sd = makeShapeData(solids);
            OpResult r;
            r.history.faceOrigins.assign(sd->faces.Extent(), {});
            tagProfileSides(ps, *sd, bf->edgeIds, r.history);
            // Caps: planar faces in the start plane (the profile plane) and in the end plane (rotated by 2π·turns).
            gp_Trsf rot;
            rot.SetRotation(gp_Ax1(gp(axis.origin), gd(a)), 2 * PI * turns);
            const gp_Dir nEnd = gd(n).Transformed(rot), uEnd = gd(u).Transformed(rot);
            int orphans = 0;
            for (int fi = 1; fi <= sd->faces.Extent(); ++fi) {
                auto& o = r.history.faceOrigins[fi - 1];
                if (!o.empty()) continue;
                const TopoDS_Face& face = TopoDS::Face(sd->faces(fi));
                BRepAdaptor_Surface su(face);
                if (su.GetType() == GeomAbs_Plane) {
                    GProp_GProps fp;
                    BRepGProp::SurfaceProperties(face, fp);
                    const Vec3 c = pt(fp.CentreOfMass()) - axis.origin;
                    const gp_Dir fn = su.Plane().Axis().Direction();
                    const bool atStart = std::abs(c.dot(n)) <= 1e-6 && c.dot(u) > 0 && std::abs(std::abs(fn.Dot(gd(n))) - 1) <= 1e-6;
                    const bool atEnd = std::abs(c.dot(Vec3(nEnd.X(), nEnd.Y(), nEnd.Z()))) <= 1e-6 && c.dot(Vec3(uEnd.X(), uEnd.Y(), uEnd.Z())) > 0 && std::abs(std::abs(fn.Dot(nEnd)) - 1) <= 1e-6;
                    if (atStart || atEnd) {
                        Origin x;
                        x.role = atStart ? ProfileRole::CapStart : ProfileRole::CapEnd;
                        o.push_back(x);
                        continue;
                    }
                }
                // A side face without Generated: the profile segment whose edge midpoint lies on it (as §9.10).
                tagSideByMidpoint(face, bf->edgeIds, o);
                if (o.empty()) ++orphans;
            }
            for (auto& o : r.history.faceOrigins) sortOrigins(o);
            if (orphans > 0) r.warnings.push_back(Error("KERNEL_HISTORY_INCOMPLETE").with("actual", double(orphans)));
            r.shape = wrap(sd);
            const double expected = turns * 2 * PI * rho * area;
            const double v = massProperties(r.shape).volume;
            if (!(v > 0) || std::abs(v - expected) > KERNEL_HELIX_VOLUME_REL * expected || !checkValidity(r.shape).valid)
                return Error("KERNEL_HELIX_FAILED");
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_HELIX_FAILED");
        }
    }

    Result<OpResult> pipeSweep(const Profile& pIn, const std::vector<PathSegment>& path, const Cancel* cancel) const override {
        // §3.26, §9.6к (FX7-PT-002)
        Profile p;
        auto info = validateProfile(pIn, &p);
        if (!info) return info.error();
        auto bad = [](const char* name) { return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string(name)); };
        auto finite = [](const Vec3& v) { return v.allFinite() && v.cwiseAbs().maxCoeff() <= KERNEL_MAX_COORD; };
        if (path.empty()) return bad("path");
        for (size_t i = 0; i < path.size(); ++i) {
            const PathSegment& g = path[i];
            if (!finite(g.a) || !finite(g.b) || (g.b - g.a).norm() < KERNEL_MIN_SEGMENT && !g.arc) return bad("path");
            if (g.arc) {
                const double ra = (g.a - g.center).norm(), rb = (g.b - g.center).norm();
                if (!finite(g.center) || !finite(g.axis) || std::abs(g.axis.norm() - 1) > 1e-9 || ra < KERNEL_MIN_DIMENSION ||
                    std::abs(ra - rb) > KERNEL_PROFILE_GAP_TOL || std::abs(g.axis.dot(g.a - g.center)) > KERNEL_PROFILE_GAP_TOL ||
                    std::abs(g.axis.dot(g.b - g.center)) > KERNEL_PROFILE_GAP_TOL)
                    return bad("path");
            }
            if (i > 0 && (path[i - 1].b - g.a).norm() > KERNEL_PROFILE_GAP_TOL) return bad("path");
        }
        const bool closed = path.size() > 1 && (path.back().b - path.front().a).norm() <= KERNEL_PROFILE_GAP_TOL;
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        auto arcAngle = [](const PathSegment& g) {
            const Vec3 ra = g.a - g.center, rb = g.b - g.center;
            double ang = std::atan2(g.axis.dot(ra.cross(rb)), ra.dot(rb));
            if (ang <= KERNEL_MIN_ANGLE) ang += 2 * PI;
            return ang;
        };
        // Distance from a point to a path segment (face → segment of the path by its centroid).
        auto segDistance = [&](const PathSegment& g, const Point3& q) {
            if (!g.arc) {
                const Vec3 d = g.b - g.a;
                const double t = std::clamp((q - g.a).dot(d) / d.squaredNorm(), 0.0, 1.0);
                return (g.a + t * d - q).norm();
            }
            const Vec3 ra = g.a - g.center;
            Vec3 w = q - g.center;
            const double h = g.axis.dot(w);
            w -= h * g.axis;
            if (w.norm() < 1e-12) return std::hypot(ra.norm(), h);
            double ang = std::atan2(g.axis.dot(ra.cross(w)), ra.dot(w));
            if (ang < 0) ang += 2 * PI;
            if (ang <= arcAngle(g)) return std::hypot(w.norm() - ra.norm(), h);
            return std::min((g.a - q).norm(), (g.b - q).norm());
        };
        try {
            // Spine: shared vertices at the joints, arcs as circles from a counterclockwise about axis.
            std::vector<TopoDS_Vertex> verts;
            for (const auto& g : path) verts.push_back(BRepBuilderAPI_MakeVertex(gp(g.a)).Vertex());
            if (!closed) verts.push_back(BRepBuilderAPI_MakeVertex(gp(path.back().b)).Vertex());
            BRepBuilderAPI_MakeWire mw;
            for (size_t i = 0; i < path.size(); ++i) {
                const PathSegment& g = path[i];
                const TopoDS_Vertex& v1 = verts[i];
                const TopoDS_Vertex& v2 = verts[(i + 1) % verts.size()];
                TopoDS_Edge e;
                if (!g.arc) {
                    BRepBuilderAPI_MakeEdge me(v1, v2);
                    if (!me.IsDone()) return Error("KERNEL_SWEEP_FAILED");
                    e = me.Edge();
                } else {
                    const gp_Circ circ(gp_Ax2(gp(g.center), gd(g.axis), gd((g.a - g.center).normalized())), (g.a - g.center).norm());
                    BRepBuilderAPI_MakeEdge me(circ, v1, v2);
                    if (!me.IsDone()) return Error("KERNEL_SWEEP_FAILED");
                    e = me.Edge();
                }
                mw.Add(e);
                if (!mw.IsDone()) return Error("KERNEL_SWEEP_FAILED");
            }
            const TopoDS_Wire spine = mw.Wire();
            // Trihedron: a planar path — fixed binormal along the path plane normal; otherwise corrected Frenet.
            std::optional<Vec3> normal;
            for (const auto& g : path)
                if (g.arc) normal = g.axis;
            if (!normal)
                for (size_t i = 0; i + 1 < path.size() && !normal; ++i)
                    for (size_t j = i + 1; j < path.size() && !normal; ++j) {
                        const Vec3 c = (path[i].b - path[i].a).normalized().cross((path[j].b - path[j].a).normalized());
                        if (c.norm() > 1e-6) normal = c.normalized();
                    }
            bool planar = normal.has_value();
            if (planar)
                for (const auto& g : path) {
                    planar = planar && std::abs(normal->dot(g.a - path.front().a)) <= KERNEL_PROFILE_GAP_TOL &&
                             std::abs(normal->dot(g.b - path.front().a)) <= KERNEL_PROFILE_GAP_TOL;
                    if (g.arc) planar = planar && g.axis.cross(*normal).norm() <= 1e-9;
                }
            // One loop of the profile → a solid with face origins (side faces carry "<profile segment>/<path segment>").
            struct Piece {
                TopoDS_Shape solid;
                std::vector<std::vector<Origin>> origins;
            };
            int orphans = 0;
            auto sweepLoop = [&](const Loop3& loop) -> Result<Piece> {
                Profile q;
                q.plane = p.plane;
                q.outer = loop;
                auto bf = buildFace(q);
                if (!bf) return Error("KERNEL_SWEEP_FAILED");
                BRepOffsetAPI_MakePipeShell ps(spine);
                if (planar) ps.SetMode(gd(*normal));
                else ps.SetMode(false);  // corrected Frenet
                ps.SetTransitionMode(BRepBuilderAPI_RightCorner);
                ps.Add(BRepTools::OuterWire(bf->face), false, false);
                ps.Build();
                if (!ps.IsDone() || !ps.MakeSolid()) return Error("KERNEL_SWEEP_FAILED");
                const auto solids = solidsOf(ps.Shape());
                if (solids.size() != 1) return Error("KERNEL_SWEEP_FAILED");
                Piece pc;
                pc.solid = solids.front();
                ShapeMap faces;
                for (TopExp_Explorer ex(pc.solid, TopAbs_FACE); ex.More(); ex.Next()) faces.Add(ex.Current());
                pc.origins.assign(faces.Extent(), {});
                auto pathOf = [&](const TopoDS_Shape& face) {
                    GProp_GProps fp;
                    BRepGProp::SurfaceProperties(face, fp);
                    const Point3 g = pt(fp.CentreOfMass());
                    size_t best = 0;
                    for (size_t i = 1; i < path.size(); ++i)
                        if (segDistance(path[i], g) < segDistance(path[best], g) - 1e-9) best = i;
                    return path[best].id;
                };
                for (const auto& [e, id] : bf->edgeIds)
                    for (const TopoDS_Shape& gsh : ps.Generated(e)) {
                        if (gsh.ShapeType() != TopAbs_FACE) continue;
                        const int fi = faces.FindIndex(gsh);
                        if (fi <= 0) continue;
                        Origin o;
                        o.segment = id + "/" + pathOf(gsh);
                        pc.origins[fi - 1].push_back(o);
                    }
                for (int fi = 1; fi <= faces.Extent(); ++fi) {
                    auto& o = pc.origins[fi - 1];
                    if (!o.empty()) continue;
                    const TopoDS_Face& face = TopoDS::Face(faces(fi));
                    BRepAdaptor_Surface su(face);
                    GProp_GProps fp;
                    BRepGProp::SurfaceProperties(face, fp);
                    const Point3 g = pt(fp.CentreOfMass());
                    if (!closed && su.GetType() == GeomAbs_Plane) {
                        // Caps: planar faces through an end of the path, normal along the path there.
                        const gp_Dir fn = su.Plane().Axis().Direction();
                        auto tangent = [&](const PathSegment& sg, const Point3& at) -> Vec3 {
                            return sg.arc ? Vec3(sg.axis.cross(at - sg.center).normalized()) : Vec3((sg.b - sg.a).normalized());
                        };
                        const Vec3 fnv(fn.X(), fn.Y(), fn.Z());
                        const Vec3 t0 = tangent(path.front(), path.front().a), t1 = tangent(path.back(), path.back().b);
                        const bool atStart = std::abs(fnv.dot(g - path.front().a)) <= 1e-6 && std::abs(std::abs(fnv.dot(t0)) - 1) <= 1e-6;
                        const bool atEnd = std::abs(fnv.dot(g - path.back().b)) <= 1e-6 && std::abs(std::abs(fnv.dot(t1)) - 1) <= 1e-6;
                        const bool startPlane = std::abs(p.plane.normal.dot(g - p.plane.origin)) <= 1e-6 &&
                                                std::abs(std::abs(fnv.dot(p.plane.normal)) - 1) <= 1e-6;
                        if (atStart || atEnd || startPlane) {
                            Origin x;
                            x.role = atStart || startPlane ? ProfileRole::CapStart : ProfileRole::CapEnd;
                            o.push_back(x);
                            continue;
                        }
                    }
                    // A side face without Generated: the profile segment whose edge midpoint (swept) lies nearest to it.
                    double bestD = 1e300;
                    std::string bestId;
                    for (const auto& [e, id] : bf->edgeIds) {
                        BRepAdaptor_Curve c(TopoDS::Edge(e));
                        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
                        BRepExtrema_DistShapeShape dd(BRepBuilderAPI_MakeVertex(mid).Vertex(), face);
                        if (dd.IsDone() && dd.Value() < bestD) bestD = dd.Value(), bestId = id;
                    }
                    if (bestD <= KERNEL_PROFILE_GAP_TOL) {
                        Origin x;
                        x.segment = bestId + "/" + pathOf(face);
                        o.push_back(x);
                    } else {
                        ++orphans;
                    }
                }
                return pc;
            };
            auto outer = sweepLoop(p.outer);
            if (!outer) return outer.error();
            auto sd = makeShapeData({outer->solid});
            OpResult r;
            r.shape = wrap(sd);
            r.history.faceOrigins = outer->origins;
            for (const auto& h : p.holes) {
                if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
                auto hole = sweepLoop(h);
                if (!hole) return hole.error();
                auto hs = wrap(makeShapeData({hole->solid}));
                auto cut = boolean(BoolKind::Subtract, r.shape, hs, cancel);
                if (!cut) return cut.error().code == "KERNEL_CANCELLED" ? cut.error() : Error("KERNEL_SWEEP_FAILED");
                std::vector<std::vector<Origin>> next(cut->history.faceOrigins.size());
                for (size_t f = 0; f < next.size(); ++f)
                    for (const auto& o : cut->history.faceOrigins[f])
                        if (o.kind == Origin::Kind::FromFace) {
                            const auto& from = o.input.operand == 0 ? r.history.faceOrigins : hole->origins;
                            if (o.input.face < int(from.size()))
                                for (const auto& x : from[size_t(o.input.face)]) next[f].push_back(x);
                        }
                r.shape = cut->shape;
                r.history.faceOrigins = std::move(next);
            }
            for (auto& o : r.history.faceOrigins) sortOrigins(o);
            if (orphans > 0) r.warnings.push_back(Error("KERNEL_HISTORY_INCOMPLETE").with("actual", double(orphans)));
            if (D(r.shape).solids.size() != 1 || !(massProperties(r.shape).volume > 0) || !checkValidity(r.shape).valid)
                return Error("KERNEL_SWEEP_FAILED");
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            return r;
        } catch (const Standard_Failure&) {
            return Error("KERNEL_SWEEP_FAILED");
        }
    }

    OpResult combine(const std::vector<Shape>& shapes) const override {
        std::vector<TopoDS_Shape> solids;
        for (const auto& s : shapes)
            for (const auto& x : D(s).solids) solids.push_back(x);
        auto sd = makeShapeData(solids);
        OpResult r;
        r.history.faceOrigins.assign(sd->faces.Extent(), {});
        for (size_t i = 0; i < shapes.size(); ++i)
            for (int f = 1; f <= D(shapes[i]).faces.Extent(); ++f) {
                const int fi = sd->faces.FindIndex(D(shapes[i]).faces(f));
                if (fi <= 0) continue;
                Origin o;
                o.kind = Origin::Kind::FromFace;
                o.input = {static_cast<int>(i), f - 1};
                r.history.faceOrigins[fi - 1].push_back(o);
            }
        r.shape = wrap(sd);
        return r;
    }

    std::vector<std::pair<Shape, std::vector<int>>> components(const Shape& s) const override {
        std::vector<std::pair<Shape, std::vector<int>>> out;
        const OcctShape& d = D(s);
        for (const auto& sol : d.solids) {
            auto sd = makeShapeData({sol});
            std::vector<int> map;
            for (int f = 1; f <= sd->faces.Extent(); ++f) map.push_back(d.faces.FindIndex(sd->faces(f)) - 1);
            out.push_back({wrap(sd), map});
        }
        return out;
    }

    KernelInfo info() const override { return {"occt", OCC_VERSION_COMPLETE, KERNEL_CONTRACT_VERSION}; }

    // ---------------------------------------------------------------- exchange (§3.16, §9.9а)

    Result<std::string> shapeToBrep(const Shape& s) const override { return occt::brepWrite(D(s).shape); }

    Result<Shape> shapeFromBrep(const std::string& text) const override {
        auto r = occt::brepRead(text);
        if (!r) return r.error();
        return wrap(makeShapeData(solidsOf(*r)));
    }

    // Dumping shapes for the cache (§3.16а, §9.9а): a compound of compound shapes, binary format without triangulation.
    Result<std::string> shapesToBlob(const std::vector<Shape>& shapes) const override {
        try {
            BRep_Builder b;
            TopoDS_Compound top;
            b.MakeCompound(top);
            for (const auto& s : shapes) {
                if (s.null()) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("shapes"));
                TopoDS_Compound c;
                b.MakeCompound(c);
                for (const auto& solid : D(s).solids) b.Add(c, solid);
                b.Add(top, c);
            }
            std::ostringstream os(std::ios::binary);
            BinTools::Write(top, os, /*withTriangles*/ false, /*withNormals*/ false, BinTools_FormatVersion_CURRENT);
            if (!os) return Error("KERNEL_INTERNAL").with("detail", std::string("blob write"));
            return os.str();
        } catch (const Standard_Failure& e) {
            return Error("KERNEL_INTERNAL").with("detail", std::string(e.GetMessageString()));
        } catch (...) {
            return Error("KERNEL_INTERNAL").with("detail", std::string("blob write"));
        }
    }

    Result<std::vector<Shape>> shapesFromBlob(const std::string& blob) const override {
        try {
            std::istringstream is(blob, std::ios::binary);
            TopoDS_Shape top;
            BinTools::Read(top, is);
            if (top.IsNull() || top.ShapeType() != TopAbs_COMPOUND)
                return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("blob structure"));
            std::vector<Shape> out;
            for (TopoDS_Iterator it(top, /*cumOri*/ false, /*cumLoc*/ false); it.More(); it.Next()) {
                const TopoDS_Shape& c = it.Value();
                if (c.ShapeType() != TopAbs_COMPOUND) return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("blob structure"));
                std::vector<TopoDS_Shape> solids;
                for (TopoDS_Iterator si(c, false, false); si.More(); si.Next()) {
                    if (si.Value().ShapeType() != TopAbs_SOLID) return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("blob structure"));
                    solids.push_back(si.Value());
                }
                out.push_back(wrap(makeShapeData(solids)));
            }
            return out;
        } catch (const Standard_Failure& e) {
            return Error("KERNEL_IMPORT_FAILED").with("detail", std::string(e.GetMessageString()));
        } catch (...) {
            return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("blob read"));
        }
    }

    Status writeStep(const std::string& path, const StepModel& model) const override {
        // Preconditions (§3.16, S4-QA-004): the root and component references are product indices, without cycles.
        const int n = static_cast<int>(model.products.size());
        if (model.root < 0 || model.root >= n)
            return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("root")).with("actual", double(model.root));
        for (const auto& p : model.products)
            for (const auto& c : p.components)
                if (c.product < 0 || c.product >= n)
                    return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("product")).with("actual", double(c.product));
        {
            std::vector<int> state(size_t(n), 0);  // 0 — not visited, 1 — on the path, 2 — done
            std::function<bool(int)> acyclic = [&](int i) {
                if (state[size_t(i)] == 1) return false;
                if (state[size_t(i)] == 2) return true;
                state[size_t(i)] = 1;
                for (const auto& c : model.products[size_t(i)].components)
                    if (!acyclic(c.product)) return false;
                state[size_t(i)] = 2;
                return true;
            };
            for (int i = 0; i < n; ++i)
                if (!acyclic(i)) return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("components")).with("detail", std::string("cycle"));
        }
        std::vector<occt::OccStepProduct> ps;
        for (const auto& p : model.products) {
            occt::OccStepProduct o;
            o.name = p.name;
            o.color = p.color;
            if (p.shape) {
                const OcctShape& d = D(*p.shape);
                o.shape = d.shape;
                for (const auto& [fi, col] : p.faceColors)
                    if (fi >= 0 && fi < d.faces.Extent()) o.faceColors.push_back({d.faces(fi + 1), col});
            }
            for (const auto& c : p.components) {
                gp_Trsf t;
                const auto& r = c.motion.r;
                t.SetValues(r[0], r[1], r[2], c.motion.t.x(), r[3], r[4], r[5], c.motion.t.y(), r[6], r[7], r[8], c.motion.t.z());
                o.components.push_back({c.product, t, c.name});
            }
            ps.push_back(std::move(o));
        }
        return occt::stepWrite(path, ps, model.root);
    }

    // ------------------------------------------------------------ solids from faces and from a mesh (§3.22, §3.23)
    Result<Shape> solidFromFaces(const SolidSpec& spec, const Cancel* cancel) const override {
        auto r = occt::buildSolidFromFaces(spec, cancel);
        if (!r) return r.error();
        return wrap(makeShapeData(solidsOf(*r)));
    }
    Result<Shape> solidFromMesh(const MeshSpec& mesh, const Cancel* cancel) const override {
        auto r = occt::buildSolidFromMesh(mesh, cancel);
        if (!r) return r.error();
        return wrap(makeShapeData(solidsOf(*r)));
    }

    Result<StepModel> readStep(const std::string& path) const override {
        auto r = occt::stepRead(path);
        if (!r) return r.error();
        StepModel m;
        m.root = r->second;
        for (const auto& o : r->first) {
            StepProduct p;
            p.name = o.name;
            p.color = o.color;
            if (!o.shape.IsNull()) {
                auto d = makeShapeData(solidsOf(o.shape));
                for (const auto& [face, col] : o.faceColors)
                    if (const int fi = d->faces.FindIndex(face); fi > 0) p.faceColors[fi - 1] = col;
                p.shape = wrap(d);
            }
            for (const auto& [prod, t, name] : o.components) {
                StepComponent c;
                c.product = prod;
                c.name = name;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) c.motion.r[size_t(3 * i + j)] = t.Value(i + 1, j + 1);
                const gp_XYZ tr = t.TranslationPart();
                c.motion.t = Vec3(tr.X(), tr.Y(), tr.Z());
                p.components.push_back(std::move(c));
            }
            m.products.push_back(std::move(p));
        }
        return m;
    }
};

}  // namespace

std::unique_ptr<Kernel> createOcctKernel() { return guardKernel(std::make_unique<OcctKernel>()); }

}  // namespace construe::kernel
