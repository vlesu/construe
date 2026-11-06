// Healing a shape with inflated entity tolerances on OCCT (spec/kernel-interface.md §3.24, §9.6з; S4-REC-013).
#include "kernel_occt/occt_build.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Geom_Line.hxx>
#include <Geom_Conic.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <GeomProjLib.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2d_Line.hxx>
#include <TopoDS_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Mat.hxx>
#include <gp_Vec.hxx>
#include <gp_Vec2d.hxx>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <vector>

namespace construe::kernel::occt {

namespace {

constexpr double HEAL_PRECISION = 1e-7;  // mm: fixing precision and initial entity tolerance (§9.6з)
constexpr int HEAL_VERTEX_ITERATIONS = 8;
constexpr double HEAL_REG = 1e-6;        // least-squares regularization (tangent edges at the vertex are parallel)
constexpr double HEAL_EDGE_TOL = 1e-5;   // mm: an edge with a larger tolerance gets its curve replaced (the former KERNEL_ENTITY_TOL_WARN)
constexpr int HEAL_EDGE_SAMPLES = 20;    // edge samples: curve-to-surface deviation and intersection curve choice
constexpr int HEAL_MARCH_SAMPLES = 32;   // points of an intersection curve built by points (marchIntersection)
constexpr int HEAL_MARCH_ITERATIONS = 50;  // alternate projections per point

// Edge end at a vertex: the curve, the parameter on it, whether this is the first end.
struct EdgeEnd {
    TopoDS_Edge edge;
    Handle(Geom_Curve) curve;
    double first = 0, last = 0;
    bool atFirst = true;
    double t = 0;
};

// Curve parameter closest to p near t0 (Newton on the squared distance; stays near the edge end).
double projectNear(const Handle(Geom_Curve)& c, const gp_Pnt& p, double t0) {
    double t = t0;
    for (int i = 0; i < 20; ++i) {
        gp_Pnt q;
        gp_Vec d1, d2;
        c->D2(t, q, d1, d2);
        const gp_Vec r(p, q);
        const double g = r.Dot(d1), h = d1.Dot(d1) + r.Dot(d2);
        if (!(std::abs(h) > 1e-300)) break;
        const double dt = g / h;
        t -= dt;
        if (!c->IsPeriodic()) t = std::clamp(t, c->FirstParameter(), c->LastParameter());
        if (std::abs(dt) < 1e-14 * (1 + std::abs(t))) break;
    }
    return t;
}

// Move the vertex to the point closest (least squares) to the curves of all its edges (§9.6з item 2); the edges get new
// ends: projections of the point. If the shift exceeds maxShift, the vertex is left untouched.
void recenterVertices(const TopoDS_Shape& sh, double maxShift, TopTools_MapOfShape& moved) {
    TopTools_IndexedDataMapOfShapeListOfShape ve;
    TopExp::MapShapesAndAncestors(sh, TopAbs_VERTEX, TopAbs_EDGE, ve);
    BRep_Builder B;
    for (int vi = 1; vi <= ve.Extent(); ++vi) {
        const TopoDS_Vertex v = TopoDS::Vertex(ve.FindKey(vi));
        std::vector<EdgeEnd> ends;
        TopTools_MapOfShape seen;
        for (TopTools_ListIteratorOfListOfShape it(ve.FindFromIndex(vi)); it.More(); it.Next()) {
            const TopoDS_Edge e = TopoDS::Edge(it.Value());
            if (!seen.Add(e.Oriented(TopAbs_FORWARD)) || BRep_Tool::Degenerated(e)) continue;
            double f = 0, l = 0;
            Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
            if (c.IsNull()) continue;
            const TopLoc_Location loc = e.Location();
            if (!loc.IsIdentity()) c = Handle(Geom_Curve)::DownCast(c->Transformed(loc.Transformation()));
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(TopoDS::Edge(e.Oriented(TopAbs_FORWARD)), v1, v2);
            if (v1.IsSame(v)) ends.push_back({e, c, f, l, true, f});
            if (v2.IsSame(v)) ends.push_back({e, c, f, l, false, l});
        }
        if (ends.empty()) continue;
        const gp_Pnt p0 = BRep_Tool::Pnt(v);
        Eigen::Vector3d p(p0.X(), p0.Y(), p0.Z());
        for (int it = 0; it < HEAL_VERTEX_ITERATIONS; ++it) {
            // Σ (I − ττᵀ)(p − q) = 0 over the tangent lines at the closest points of the curves.
            Eigen::Matrix3d A = HEAL_REG * Eigen::Matrix3d::Identity();
            Eigen::Vector3d b = HEAL_REG * p;
            for (auto& en : ends) {
                en.t = projectNear(en.curve, gp_Pnt(p.x(), p.y(), p.z()), en.t);
                gp_Pnt q;
                gp_Vec d1;
                en.curve->D1(en.t, q, d1);
                Eigen::Vector3d tau(d1.X(), d1.Y(), d1.Z());
                if (tau.norm() < 1e-300) continue;
                tau.normalize();
                const Eigen::Matrix3d M = Eigen::Matrix3d::Identity() - tau * tau.transpose();
                A += M;
                b += M * Eigen::Vector3d(q.X(), q.Y(), q.Z());
            }
            const Eigen::Vector3d np = A.ldlt().solve(b);
            const double moved = (np - p).norm();
            p = np;
            if (moved < 1e-12) break;
        }
        if (!((p - Eigen::Vector3d(p0.X(), p0.Y(), p0.Z())).norm() <= maxShift)) continue;
        const gp_Pnt np(p.x(), p.y(), p.z());
        double tol = HEAL_PRECISION;
        for (auto& en : ends) {
            en.t = projectNear(en.curve, np, en.t);
            tol = std::max(tol, en.curve->Value(en.t).Distance(np));
        }
        B.UpdateVertex(v, np, tol);
        // New edge ends (range from the projection parameters).
        for (const auto& en : ends) {
            double f = 0, l = 0;
            BRep_Tool::Range(en.edge, f, l);
            if (en.atFirst) f = en.t;
            else l = en.t;
            if (l > f) B.Range(en.edge, f, l);
            moved.Add(en.edge);
        }
    }
}

// New pcurve pc (on [f, l]) on a periodic surface: shifted by whole periods towards the old pcurve old (on [pf, pl]).
void alignPeriod(const Handle(Geom2d_Curve)& pc, double f, double l, const Handle(Geom2d_Curve)& old, double pf, double pl,
                 const Handle(Geom_Surface)& surf) {
    if (old.IsNull()) return;
    const gp_Pnt2d a = old->Value(0.5 * (pf + pl)), b = pc->Value(0.5 * (f + l));
    double du = 0, dv = 0;
    if (surf->IsUPeriodic()) du = std::round((a.X() - b.X()) / surf->UPeriod()) * surf->UPeriod();
    if (surf->IsVPeriodic()) dv = std::round((a.Y() - b.Y()) / surf->VPeriod()) * surf->VPeriod();
    if (du != 0 || dv != 0) pc->Translate(gp_Vec2d(du, dv));
}

// Pcurves of edges with changed ends (§9.6з item 3) are rebuilt by projecting the 3D curve onto the face surface; on
// a periodic surface the pcurve is shifted by whole periods towards the old one (the face loop stays closed in uv).
// Seams are left untouched (their pcurves are isolines; only their range changes).
void reprojectPCurves(const TopoDS_Shape& sh, const TopTools_MapOfShape& edges) {
    TopTools_IndexedDataMapOfShapeListOfShape ef;
    TopExp::MapShapesAndAncestors(sh, TopAbs_EDGE, TopAbs_FACE, ef);
    BRep_Builder B;
    for (TopTools_MapOfShape::Iterator it(edges); it.More(); it.Next()) {
        const TopoDS_Edge e = TopoDS::Edge(it.Value());
        const int idx = ef.FindIndex(e);
        if (idx == 0 || BRep_Tool::Degenerated(e)) continue;
        double f = 0, l = 0;
        Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
        if (c.IsNull() || !(l > f)) continue;
        TopTools_MapOfShape done;
        for (TopTools_ListIteratorOfListOfShape fi(ef.FindFromIndex(idx)); fi.More(); fi.Next()) {
            const TopoDS_Face face = TopoDS::Face(fi.Value());
            if (!done.Add(face) || BRep_Tool::IsClosed(e, face)) continue;
            try {
                double pf = 0, pl = 0;
                const Handle(Geom2d_Curve) old = BRep_Tool::CurveOnSurface(e, face, pf, pl);
                const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
                double reached = 0;
                Handle(Geom2d_Curve) pc = GeomProjLib::Curve2d(c, f, l, surf, reached);
                if (pc.IsNull() || !(reached <= HEAL_EDGE_TOL)) continue;
                alignPeriod(pc, f, l, old, pf, pl, surf);
                B.UpdateEdge(e, pc, face, HEAL_PRECISION);
                B.Range(e, face, f, l);
            } catch (const Standard_Failure&) {
            }
        }
    }
}

// Largest distance from the points of curve c on [f, l] to surface s.
double curveToSurface(const Handle(Geom_Curve)& c, double f, double l, const Handle(Geom_Surface)& s) {
    double d = 0;
    for (int i = 0; i <= HEAL_EDGE_SAMPLES; ++i) {
        GeomAPI_ProjectPointOnSurf pr(c->Value(f + (l - f) * i / HEAL_EDGE_SAMPLES), s);
        if (pr.NbPoints() == 0) return 1e100;
        d = std::max(d, pr.LowerDistance());
    }
    return d;
}

void refitEdge(const BRep_Builder& B, const TopoDS_Edge& e, const TopTools_ListOfShape& edgeFaces, double maxTol);

// Intersection curve of s1 and s2 near the edge curve c on [f, l] by points (§9.6з item 2, when the surfaces'
// intersection has no curve along the edge): HEAL_MARCH_SAMPLES + 1 points of c (the ends — the edge's vertices v0, v1),
// each moved onto both surfaces by
// alternate projection (until the projections are closer than HEAL_PRECISION, at most HEAL_MARCH_ITERATIONS), a spline
// through them (GeomAPI_PointsToBSpline, tolerance HEAL_PRECISION·10); null if a point does not converge.
Handle(Geom_Curve) marchIntersection(const Handle(Geom_Curve)& c, double f, double l, const gp_Pnt& v0, const gp_Pnt& v1,
                                     const Handle(Geom_Surface)& s1, const Handle(Geom_Surface)& s2) {
    TColgp_Array1OfPnt pts(1, HEAL_MARCH_SAMPLES + 1);
    for (int i = 0; i <= HEAL_MARCH_SAMPLES; ++i) {
        // The ends start from the edge's vertices: the curve ends at the intersection points nearest to them.
        gp_Pnt p = i == 0 ? v0 : i == HEAL_MARCH_SAMPLES ? v1 : c->Value(f + (l - f) * i / HEAL_MARCH_SAMPLES);
        bool done = false;
        for (int it = 0; it < HEAL_MARCH_ITERATIONS && !done; ++it) {
            GeomAPI_ProjectPointOnSurf p1(p, s1);
            if (p1.NbPoints() == 0) return nullptr;
            GeomAPI_ProjectPointOnSurf p2(p1.NearestPoint(), s2);
            if (p2.NbPoints() == 0) return nullptr;
            done = p2.LowerDistance() <= HEAL_PRECISION;
            p = p2.NearestPoint();
        }
        if (!done) return nullptr;
        pts.SetValue(i + 1, p);
    }
    GeomAPI_PointsToBSpline fit(pts, 3, 8, GeomAbs_C2, HEAL_PRECISION * 10);
    if (!fit.IsDone()) return nullptr;
    return Handle(Geom_Curve)(fit.Curve());
}

// Face seam (§9.6з item 2): pcurves are isolines of the periodic surface direction; a curve deviating from the surface
// is replaced by the exact isoline (UIso/VIso), and the pcurves by lines with the same parameterization.
void refitSeam(const BRep_Builder& B, const TopoDS_Edge& e, const TopoDS_Face& face) {
    double f = 0, l = 0;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
    const Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    if (c.IsNull() || !(l > f) || curveToSurface(c, f, l, s) <= HEAL_PRECISION) return;
    double pf = 0, pl = 0;
    const Handle(Geom2d_Line) p1 = Handle(Geom2d_Line)::DownCast(BRep_Tool::CurveOnSurface(TopoDS::Edge(e.Oriented(TopAbs_FORWARD)), face, pf, pl));
    const Handle(Geom2d_Line) p2 = Handle(Geom2d_Line)::DownCast(BRep_Tool::CurveOnSurface(TopoDS::Edge(e.Oriented(TopAbs_REVERSED)), face, pf, pl));
    if (p1.IsNull() || p2.IsNull()) return;
    const gp_Dir2d dir = p1->Direction();
    const bool alongV = std::abs(dir.X()) < 1e-12, alongU = std::abs(dir.Y()) < 1e-12;
    if (!alongV && !alongU) return;
    const double sgn = (alongV ? dir.Y() : dir.X()) > 0 ? 1 : -1;
    // Parameter along the seam at the start and end of the edge.
    const gp_Pnt2d a = p1->Value(f), b = p1->Value(l);
    const double w0 = sgn * (alongV ? a.Y() : a.X()), w1 = sgn * (alongV ? b.Y() : b.X());
    if (!(w1 > w0)) return;
    Handle(Geom_Curve) iso = alongV ? s->UIso(a.X()) : s->VIso(a.Y());
    if (sgn < 0) iso = iso->Reversed();
    // Pcurve lines with parameter w: point (u_i, 0) or (0, v_i), direction ±axis.
    auto line = [&](const Handle(Geom2d_Line)& p) -> Handle(Geom2d_Curve) {
        const gp_Pnt2d o = p->Value(f);
        return alongV ? new Geom2d_Line(gp_Pnt2d(o.X(), 0), gp_Dir2d(0, sgn)) : new Geom2d_Line(gp_Pnt2d(0, o.Y()), gp_Dir2d(sgn, 0));
    };
    B.UpdateEdge(e, iso, TopLoc_Location(), HEAL_PRECISION);
    B.UpdateEdge(TopoDS::Edge(e.Oriented(TopAbs_FORWARD)), line(p1), line(p2), face, HEAL_PRECISION);  // pcurves for FORWARD and REVERSED
    B.Range(e, w0, w1);
}

// An edge curve deviating from the surface of one of its two faces by more than HEAL_PRECISION is replaced by the
// face surfaces' intersection curve closest to the edge (§9.6з item 2); pcurves on the faces are re-projected.
void refitEdges(const TopoDS_Shape& sh, double maxTol) {
    TopTools_IndexedDataMapOfShapeListOfShape ef;
    TopExp::MapShapesAndAncestors(sh, TopAbs_EDGE, TopAbs_FACE, ef);
    BRep_Builder B;
    for (int ei = 1; ei <= ef.Extent(); ++ei) {
        const TopoDS_Edge e = TopoDS::Edge(ef.FindKey(ei));
        if (BRep_Tool::Degenerated(e) || BRep_Tool::Tolerance(e) <= HEAL_EDGE_TOL) continue;
        try {
            refitEdge(B, e, ef.FindFromIndex(ei), maxTol);
        } catch (const Standard_Failure&) {  // the edge stays as it was; the result is checked by the caller
        }
    }
}

void refitEdge(const BRep_Builder& B, const TopoDS_Edge& e, const TopTools_ListOfShape& edgeFaces, double maxTol) {
    std::vector<TopoDS_Face> faces;
    for (TopTools_ListIteratorOfListOfShape it(edgeFaces); it.More(); it.Next()) {
        const TopoDS_Face f = TopoDS::Face(it.Value());
        bool dup = false;
        for (const auto& g : faces) dup = dup || g.IsSame(f);
        if (!dup) faces.push_back(f);
    }
    if (faces.size() == 1 && BRep_Tool::IsClosed(e, faces[0])) {
        refitSeam(B, e, faces[0]);
        return;
    }
    if (faces.size() != 2) return;  // a non-manifold edge is left untouched
    double f = 0, l = 0;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
    if (c.IsNull() || !(l > f)) return;
    const Handle(Geom_Surface) s1 = BRep_Tool::Surface(faces[0]), s2 = BRep_Tool::Surface(faces[1]);
    if (curveToSurface(c, f, l, s1) <= HEAL_PRECISION && curveToSurface(c, f, l, s2) <= HEAL_PRECISION) return;
    GeomAPI_IntSS inter(s1, s2, HEAL_PRECISION);
    if (!inter.IsDone()) return;
    // Intersection curve closest to the edge points (all points no farther than maxTol): only exact curves (line,
    // conic) — approximate intersection splines are split arbitrarily and are no better than the edge.
    Handle(Geom_Curve) best;
    double bestDev = maxTol;
    for (int k = 1; k <= inter.NbLines(); ++k) {
        Handle(Geom_Curve) ck = inter.Line(k);
        // Trimmed curve: take its basis (edge end parameters may lie outside the trim).
        while (auto tc = Handle(Geom_TrimmedCurve)::DownCast(ck)) ck = tc->BasisCurve();
        if (!ck->IsKind(STANDARD_TYPE(Geom_Line)) && !ck->IsKind(STANDARD_TYPE(Geom_Conic))) continue;
        double dev = 0;
        for (int i = 0; i <= HEAL_EDGE_SAMPLES && dev <= bestDev; ++i) {
            GeomAPI_ProjectPointOnCurve pr(c->Value(f + (l - f) * i / HEAL_EDGE_SAMPLES), ck);
            dev = pr.NbPoints() > 0 ? std::max(dev, pr.LowerDistance()) : 1e100;
        }
        if (dev <= bestDev) best = ck, bestDev = dev;
    }
    double u0 = 0, u1 = 0;
    if (best.IsNull()) {
        // No exact curve (a cylinder–cylinder intersection of cross holes): the intersection by points from vertex to
        // vertex along the edge (S4-REC-023), the whole new curve is the edge.
        TopoDS_Vertex a, b;
        TopExp::Vertices(TopoDS::Edge(e.Oriented(TopAbs_FORWARD)), a, b);
        if (a.IsNull() || b.IsNull()) return;
        best = marchIntersection(c, f, l, BRep_Tool::Pnt(a), BRep_Tool::Pnt(b), s1, s2);
        if (best.IsNull()) return;
        u0 = best->FirstParameter(), u1 = best->LastParameter();
    } else {
        // End parameters on the new curve; direction same as the old curve.
        auto param = [&](const gp_Pnt& p) {
            GeomAPI_ProjectPointOnCurve pr(p, best);
            return pr.NbPoints() > 0 ? pr.LowerDistanceParameter() : 0.0;
        };
        gp_Pnt p0;
        gp_Vec d0;
        c->D1(f, p0, d0);
        u0 = param(p0);
        gp_Pnt q0;
        gp_Vec dq;
        best->D1(u0, q0, dq);
        if (dq.Dot(d0) < 0) {
            best = best->Reversed();
            u0 = param(p0);
        }
        u1 = param(c->Value(l));
        if (best->IsPeriodic()) {
            const double T = best->Period();
            while (u1 <= u0) u1 += T;
            while (u1 - u0 > T) u1 -= T;
        }
    }
    if (!(u1 > u0)) return;
    // Pcurves on the faces are projections of the new curve with the same parameterization (GeomProjLib).
    Handle(Geom2d_Curve) pc[2];
    for (int i = 0; i < 2; ++i) {
        double reached = 0;
        const Handle(Geom_Surface) surf = BRep_Tool::Surface(faces[size_t(i)]);
        pc[i] = GeomProjLib::Curve2d(best, u0, u1, surf, reached);
        if (pc[i].IsNull() || !(reached <= HEAL_EDGE_TOL)) return;
        double pf = 0, pl = 0;
        alignPeriod(pc[i], u0, u1, BRep_Tool::CurveOnSurface(e, faces[size_t(i)], pf, pl), pf, pl, surf);
    }
    B.UpdateEdge(e, best, TopLoc_Location(), HEAL_PRECISION);
    for (int i = 0; i < 2; ++i) B.UpdateEdge(e, pc[i], faces[size_t(i)], HEAL_PRECISION);
    B.Range(e, u0, u1);
}

}  // namespace

std::optional<TopoDS_Shape> healShape(const TopoDS_Shape& in, double maxTolerance) {
    try {
        TopoDS_Shape sh = BRepBuilderAPI_Copy(in, /*copyGeom*/ true, /*copyMesh*/ false).Shape();
        // 1. General fixing (§9.6з item 1).
        Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(sh);
        fix->SetPrecision(HEAL_PRECISION);
        fix->SetMinTolerance(HEAL_PRECISION);
        fix->SetMaxTolerance(maxTolerance);
        fix->Perform();
        sh = fix->Shape();
        // 2. Edge curves onto the face surfaces; vertices onto the edge curves.
        refitEdges(sh, maxTolerance);
        TopTools_MapOfShape moved;
        recenterVertices(sh, maxTolerance, moved);
        reprojectPCurves(sh, moved);
        // 3. Tolerances recomputed from the actual deviations.
        ShapeFix_ShapeTolerance().SetTolerance(sh, HEAL_PRECISION);
        // Per edge: a failure on one edge (its tolerance stays HEAL_PRECISION; the caller's check rejects the shape) does
        // not interrupt the others.
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(sh, TopAbs_EDGE, edges);
        BRep_Builder B;
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge& e = TopoDS::Edge(edges(i));
            try {
                B.SameParameter(e, false);
                BRepLib::SameParameter(e, HEAL_PRECISION);
            } catch (const Standard_Failure&) {
            }
        }
        BRepLib::UpdateTolerances(sh, /*verifyFaceTolerance*/ true);
        // 4. Final fixing of face wires (closure of pcurves in the face parameters) with a small tolerance.
        Handle(ShapeFix_Shape) fin = new ShapeFix_Shape(sh);
        fin->SetPrecision(HEAL_PRECISION);
        fin->SetMinTolerance(HEAL_PRECISION);
        fin->SetMaxTolerance(HEAL_EDGE_TOL);
        fin->Perform();
        sh = fin->Shape();
        return sh;
    } catch (const Standard_Failure&) {
        return std::nullopt;
    }
}

}  // namespace construe::kernel::occt
