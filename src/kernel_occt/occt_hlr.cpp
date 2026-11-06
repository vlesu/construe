// Hidden line removal projection on OCCT (spec/kernel-interface.md §3.17, §9.9б).
#include "kernel_occt/occt_hlr.h"

#include "base/geom2.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <HLRBRep_PolyAlgo.hxx>
#include <HLRBRep_PolyHLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <cmath>
#include <string>

namespace construe::kernel::occt {

namespace {

// Result edge (lies in the z = 0 plane of the projector frame: x = u, y = v) → output curve (§3.17 "Response curves").
void addEdge(const TopoDS_Edge& e, double chordTol, std::vector<ViewCurve>& out) {
    BRepAdaptor_Curve c(e);
    const double f = c.FirstParameter(), l = c.LastParameter();
    if (!(l > f)) return;
    if (GCPnts_AbscissaPoint::Length(c, f, l) <= KERNEL_HLR_MIN_LENGTH) return;
    auto p2 = [](const gp_Pnt& p) { return Point2(p.X(), p.Y()); };
    ViewCurve vc;
    if (c.GetType() == GeomAbs_Line) {
        vc.kind = ViewCurve::Kind::Line;
        vc.p1 = p2(c.Value(f));
        vc.p2 = p2(c.Value(l));
        out.push_back(std::move(vc));
        return;
    }
    if (c.GetType() == GeomAbs_Circle) {
        const gp_Circ ci = c.Circle();
        const double az = ci.Axis().Direction().Z();
        if (1 - std::abs(az) <= KERNEL_HLR_PARALLEL_TOL) {
            vc.center = p2(ci.Location());
            vc.radius = ci.Radius();
            const double span = l - f;
            if (span >= 2 * PI - KERNEL_FULL_TURN_TOL) {
                vc.kind = ViewCurve::Kind::Circle;
            } else {
                // The circle parameter grows counterclockwise about its axis: axis −Z means a clockwise arc, ends swapped.
                const gp_Pnt a = c.Value(az > 0 ? f : l);
                vc.kind = ViewCurve::Kind::Arc;
                vc.start = geom2::normalizeAngleSym(std::atan2(a.Y() - ci.Location().Y(), a.X() - ci.Location().X()));
                vc.end = vc.start + span;
            }
            out.push_back(std::move(vc));
            return;
        }
    }
    vc.kind = ViewCurve::Kind::Polyline;
    GCPnts_QuasiUniformDeflection d(c, chordTol, f, l);
    if (d.IsDone() && d.NbPoints() >= 2) {
        for (int i = 1; i <= d.NbPoints(); ++i) vc.points.push_back(p2(d.Value(i)));
    } else {
        vc.points = {p2(c.Value(f)), p2(c.Value(l))};
    }
    vc.points.front() = p2(c.Value(f));  // ends are the exact ends of the segment
    vc.points.back() = p2(c.Value(l));
    out.push_back(std::move(vc));
}

void collect(TopoDS_Shape comp, double chordTol, std::vector<ViewCurve>& out) {
    if (comp.IsNull()) return;
    BRepLib::BuildCurves3d(comp);
    for (TopExp_Explorer ex(comp, TopAbs_EDGE); ex.More(); ex.Next()) addEdge(TopoDS::Edge(ex.Current()), chordTol, out);
}

}  // namespace

void ensureSharedStatics() {
    static const bool created = [] {
        (void)BRepLib::Plane();
        return true;
    }();
    (void)created;
}

Result<HlrProjection> hlrProject(const std::vector<std::pair<TopoDS_Shape, gp_Trsf>>& items, const ViewDirection& view,
                                 const HlrParams& params, const Cancel* cancel) {
    ensureSharedStatics();
    HlrProjection out;
    if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
    if (items.empty()) return out;
    const Vec3 right = view.up.cross(view.toViewer);
    const gp_Ax2 ax(gp_Pnt(0, 0, 0), gp_Dir(view.toViewer.x(), view.toViewer.y(), view.toViewer.z()),
                    gp_Dir(right.x(), right.y(), right.z()));
    const HLRAlgo_Projector projector(ax);
    // Scene: copies of the topology (continuity marking writes into TShape), in the body placements (§9.9б).
    std::vector<TopoDS_Shape> scene;
    try {
        for (const auto& [shape, trsf] : items) {
            TopoDS_Shape copy = BRepBuilderAPI_Copy(shape, /*copyGeom*/ false, /*copyMesh*/ false).Shape();
            BRepLib::EncodeRegularity(copy, KERNEL_TANGENT_EDGE_ANGLE);
            copy.Move(TopLoc_Location(trsf));
            scene.push_back(copy);
        }
    } catch (const Standard_Failure& e) {
        return Error("KERNEL_HLR_FAILED").with("detail", std::string(e.what()));
    }
    std::string exactFailure;
    try {
        occ::handle<HLRBRep_Algo> algo = new HLRBRep_Algo();
        for (const auto& s : scene) algo->Add(s);
        algo->Projector(projector);
        algo->Update();
        algo->Hide();
        HLRBRep_HLRToShape h(algo);
        collect(h.VCompound(), params.chordTol, out.visible);
        collect(h.OutLineVCompound(), params.chordTol, out.visible);
        collect(h.HCompound(), params.chordTol, out.hidden);
        collect(h.OutLineHCompound(), params.chordTol, out.hidden);
        collect(h.Rg1LineVCompound(), params.chordTol, out.visibleSmooth);
        collect(h.Rg1LineHCompound(), params.chordTol, out.hiddenSmooth);
        promoteOuterContour(out);  // §3.17 "Outer contour" (FX7-DRW-001)
        return out;
    } catch (const Standard_Failure& e) {
        exactFailure = e.what();
    }
    // Approximate algorithm on the triangulation (§3.17, §9.9б).
    if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
    out = HlrProjection{};
    try {
        TopoDS_Compound comp;
        BRep_Builder b;
        b.MakeCompound(comp);
        for (const auto& s : scene) b.Add(comp, s);
        BRepMesh_IncrementalMesh mesh(comp, params.chordTol, false, KERNEL_HLR_POLY_ANGLE);
        occ::handle<HLRBRep_PolyAlgo> algo = new HLRBRep_PolyAlgo();
        algo->Load(comp);
        algo->Projector(projector);
        algo->Update();
        HLRBRep_PolyHLRToShape h;
        h.Update(algo);
        collect(h.VCompound(), params.chordTol, out.visible);
        collect(h.OutLineVCompound(), params.chordTol, out.visible);
        collect(h.HCompound(), params.chordTol, out.hidden);
        collect(h.OutLineHCompound(), params.chordTol, out.hidden);
        collect(h.Rg1LineVCompound(), params.chordTol, out.visibleSmooth);
        collect(h.Rg1LineHCompound(), params.chordTol, out.hiddenSmooth);
    } catch (const Standard_Failure& e) {
        return Error("KERNEL_HLR_FAILED").with("detail", std::string(e.what()));
    }
    promoteOuterContour(out);
    out.warnings.push_back(Error("KERNEL_HLR_APPROXIMATE").with("detail", exactFailure));
    return out;
}

}  // namespace construe::kernel::occt
