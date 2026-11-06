// Building solids from a face description and from a mesh on OCCT (spec/kernel-interface.md §3.22, §3.23, §9.6ж).
#include "kernel_occt/occt_build.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <numbers>
#include <string>
#include <vector>

namespace construe::kernel::occt {

namespace {

gp_Pnt P(const Point3& p) { return gp_Pnt(p.x(), p.y(), p.z()); }
gp_Dir Dn(const Vec3& v) { return gp_Dir(v.x(), v.y(), v.z()); }

Error buildError(const std::string& detail) { return Error("KERNEL_BUILD_FAILED").with("detail", detail); }

Vec3 refX(const Vec3& d) { return seamDirection(d); }  // §3.22: parameter origin of surfaces of revolution

Handle(Geom_Surface) makeSurface(const SurfaceSpec& s) {
    const Vec3 d = s.dir.normalized();
    gp_Ax3 ax(P(s.point), Dn(d), Dn(refX(d)));
    switch (s.kind) {
        case SurfaceInfo::Kind::Plane: return new Geom_Plane(ax);
        case SurfaceInfo::Kind::Cylinder: return new Geom_CylindricalSurface(ax, s.radius);
        case SurfaceInfo::Kind::Cone: {
            // Reference circle: at a distance of 1 mm from the apex along the axis (radius tan α).
            gp_Ax3 c(P(s.point + d), Dn(d), Dn(refX(d)));
            return new Geom_ConicalSurface(c, s.halfAngle, std::tan(s.halfAngle));
        }
        case SurfaceInfo::Kind::Sphere: return new Geom_SphericalSurface(ax, s.radius);
        case SurfaceInfo::Kind::Torus: return new Geom_ToroidalSurface(ax, s.radius, s.minorRadius);
        default: return {};
    }
}

// Edge on the exact intersection curve of the surfaces (§9.6ж item 2): the GeomAPI_IntSS branch passing through the
// description points, trimmed by the vertices; direction given by the midpoint. No such branch: empty.
std::optional<TopoDS_Edge> intersectionEdge(const EdgeSpec& e, const std::vector<Handle(Geom_Surface)>& surfs, const TopoDS_Vertex& V1,
                                            const TopoDS_Vertex& V2, const Point3& p1, const Point3& p2, double tol) {
    const auto& c = e.curve;
    if (c.surfaceA < 0 || c.surfaceB < 0 || size_t(c.surfaceA) >= surfs.size() || size_t(c.surfaceB) >= surfs.size() || c.points.size() < 3)
        return std::nullopt;
    GeomAPI_IntSS ss(surfs[size_t(c.surfaceA)], surfs[size_t(c.surfaceB)], 1e-7);
    if (!ss.IsDone()) return std::nullopt;
    const Point3 pm = c.points[c.points.size() / 2];
    for (int i = 1; i <= ss.NbLines(); ++i) {
        Handle(Geom_Curve) g = ss.Line(i);
        // A closed non-periodic branch (traversal spline) is made periodic so that the edge can start at the vertex.
        if (e.v1 == e.v2 && !g->IsPeriodic() && g->IsClosed()) {
            Handle(Geom_BSplineCurve) b = Handle(Geom_BSplineCurve)::DownCast(g->Copy());
            if (b.IsNull()) continue;
            b->SetPeriodic();
            g = b;
        }
        auto param = [&](const Point3& q, double* t) {
            GeomAPI_ProjectPointOnCurve pr(P(q), g);
            if (pr.NbPoints() == 0 || pr.LowerDistance() > 100 * tol) return false;
            *t = pr.LowerDistanceParameter();
            return true;
        };
        bool all = true;
        for (const auto& q : c.points) {
            double t;
            if (!param(q, &t)) {
                all = false;
                break;
            }
        }
        double t1, tm, t2;
        if (!all || !param(p1, &t1) || !param(pm, &tm) || !param(p2, &t2)) continue;
        const bool closed = e.v1 == e.v2;
        if (g->IsPeriodic()) {
            const double per = g->Period();
            auto wrap = [&](double t) {
                while (t < t1) t += per;
                while (t >= t1 + per) t -= per;
                return t;
            };
            tm = wrap(tm);
            t2 = closed ? t1 + per : wrap(t2);
            if (!closed && tm > t2) {  // reverse direction
                g = g->Reversed();
                if (!param(p1, &t1) || !param(pm, &tm) || !param(p2, &t2)) continue;
                tm = wrap(tm);
                t2 = wrap(t2);
                if (tm > t2) continue;
            }
        } else {
            if (closed) continue;
            if (t1 > tm && tm > t2) {
                g = g->Reversed();
                if (!param(p1, &t1) || !param(pm, &tm) || !param(p2, &t2)) continue;
            }
            if (!(t1 < tm && tm < t2)) continue;
        }
        BRepBuilderAPI_MakeEdge mk(g, V1, V2, t1, t2);
        if (mk.IsDone()) return mk.Edge();
    }
    return std::nullopt;
}

Result<TopoDS_Edge> makeEdge(const EdgeSpec& e, const std::vector<TopoDS_Vertex>& vs, const std::vector<Point3>& pts,
                             const std::vector<Handle(Geom_Surface)>& surfs, double tol) {
    const TopoDS_Vertex &V1 = vs[size_t(e.v1)], &V2 = vs[size_t(e.v2)];
    const Point3 &p1 = pts[size_t(e.v1)], &p2 = pts[size_t(e.v2)];
    switch (e.curve.kind) {
        case CurveSpec::Kind::Line: {
            BRepBuilderAPI_MakeEdge mk(V1, V2);
            if (!mk.IsDone()) break;
            return mk.Edge();
        }
        case CurveSpec::Kind::Circle: {
            const Vec3 a = e.curve.axis.normalized();
            Vec3 x = (p1 - e.curve.center) - (p1 - e.curve.center).dot(a) * a;
            if (x.norm() < 1e-12) break;
            x.normalize();
            const gp_Circ c(gp_Ax2(P(e.curve.center), Dn(a), Dn(x)), e.curve.radius);
            double u2 = 2 * std::numbers::pi;
            if (e.v1 != e.v2) {
                const Vec3 q = p2 - e.curve.center;
                u2 = std::atan2(q.dot(a.cross(x)), q.dot(x));
                if (u2 <= 0) u2 += 2 * std::numbers::pi;
            }
            Handle(Geom_Circle) gc = new Geom_Circle(c);
            BRepBuilderAPI_MakeEdge mk(gc, V1, V2, 0, u2);
            if (mk.IsDone()) return mk.Edge();
            // Vertex off the circle by more than its tolerance (a vertex of tangent arcs is solved approximately, §9.6ж item 2а):
            // the vertex tolerance is raised to their distance from the circle if it does not exceed tol·10.
            double dmax = 0;
            for (const Point3* p : {&p1, &p2}) {
                GeomAPI_ProjectPointOnCurve pc(P(*p), gc);
                if (pc.NbPoints() == 0) {
                    dmax = std::numeric_limits<double>::infinity();
                    break;
                }
                dmax = std::max(dmax, pc.LowerDistance());
            }
            if (!(dmax <= tol * 10)) break;
            BRep_Builder bb;
            for (const TopoDS_Vertex* V : {&V1, &V2})
                if (BRep_Tool::Tolerance(*V) < dmax * 1.01) bb.UpdateVertex(*V, dmax * 1.01);
            BRepBuilderAPI_MakeEdge mk2(gc, V1, V2, 0, u2);
            if (!mk2.IsDone()) break;
            return mk2.Edge();
        }
        case CurveSpec::Kind::Interp: {
            if (auto x = intersectionEdge(e, surfs, V1, V2, p1, p2, tol)) return *x;
            const bool closed = e.v1 == e.v2;
            const auto& q = e.curve.points;
            if (q.size() < (closed ? 3u : 2u)) break;
            Handle(TColgp_HArray1OfPnt) arr = new NCollection_HArray1<gp_Pnt>(1, int(q.size()));
            for (size_t i = 0; i < q.size(); ++i) arr->SetValue(int(i) + 1, P(q[i]));
            GeomAPI_Interpolate in(arr, closed, 1e-9);
            in.Perform();
            if (!in.IsDone()) break;
            Handle(Geom_BSplineCurve) c = in.Curve();
            BRepBuilderAPI_MakeEdge mk(c, V1, V2, c->FirstParameter(), c->LastParameter());
            if (!mk.IsDone()) break;
            return mk.Edge();
        }
    }
    return buildError("edge");
}

// Point inside the face: classifier on the projection of the point inside onto the surface (§9.6ж item 4).
bool insideFace(const TopoDS_Face& f, const Point3& p, double tol) {
    Handle(Geom_Surface) s = BRep_Tool::Surface(f);
    GeomAPI_ProjectPointOnSurf pr(P(p), s);
    if (!pr.IsDone() || pr.NbPoints() == 0) return false;
    double u, v;
    pr.LowerDistanceParameters(u, v);
    BRepClass_FaceClassifier cl(f, gp_Pnt2d(u, v), tol);
    return cl.State() == TopAbs_IN;
}

// Face of a sphere or cone bounded by a single coaxial circle and containing the pole or apex (§9.6ж item 3а):
// the parameter domain is a rectangle from the circle to the singular point (MakeFace makes the degenerate edge, seam).
std::optional<TopoDS_Face> capFace(const FaceSpec& fs, const SolidSpec& spec, double tol) {
    const SurfaceSpec& ss = spec.surfaces[size_t(fs.surface)];
    if ((ss.kind != SurfaceInfo::Kind::Sphere && ss.kind != SurfaceInfo::Kind::Cone) || fs.loops.size() != 1 || fs.loops[0].size() != 1)
        return std::nullopt;
    const EdgeSpec& e = spec.edges[size_t(fs.loops[0][0].edge)];
    if (e.curve.kind != CurveSpec::Kind::Circle || e.v1 != e.v2) return std::nullopt;
    const Vec3 a = e.curve.axis.normalized();
    const Vec3 w = e.curve.center - ss.point;
    if ((w - w.dot(a) * a).norm() > tol) return std::nullopt;  // circle center lies on the axis
    Handle(Geom_Surface) surf;
    double v0 = 0, v1 = 0;
    if (ss.kind == SurfaceInfo::Kind::Sphere) {
        surf = new Geom_SphericalSurface(gp_Ax3(P(ss.point), Dn(a), Dn(seamDirection(a))), ss.radius);
        const double h = w.dot(a) / ss.radius;
        if (std::abs(h) >= 1) return std::nullopt;
        const double vc = std::asin(h), hi = (fs.inside - ss.point).dot(a);
        if (hi > w.dot(a)) v0 = vc, v1 = std::numbers::pi / 2;
        else v0 = -std::numbers::pi / 2, v1 = vc;
    } else {
        const Vec3 d = ss.dir.normalized();
        if (std::abs(std::abs(a.dot(d)) - 1) > 1e-9) return std::nullopt;
        surf = makeSurface(ss);
        const double t = w.dot(d), ti = (fs.inside - ss.point).dot(d), c = std::cos(ss.halfAngle);
        if (!(ti < t)) return std::nullopt;  // the apex must be inside the face
        v0 = -1 / c;
        v1 = (t - 1) / c;
    }
    BRepBuilderAPI_MakeFace mk(surf, 0, 2 * std::numbers::pi, v0, v1, tol);
    if (!mk.IsDone()) return std::nullopt;
    TopoDS_Face f = mk.Face();
    if (!insideFace(f, fs.inside, tol)) return std::nullopt;
    double u, v;
    GeomAPI_ProjectPointOnSurf pr(P(fs.inside), surf);
    if (!pr.IsDone() || pr.NbPoints() == 0) return std::nullopt;
    pr.LowerDistanceParameters(u, v);
    GeomLProp_SLProps props(surf, u, v, 1, 1e-9);
    if (!props.IsNormalDefined()) return std::nullopt;
    const gp_Dir n = props.Normal();
    if (n.X() * fs.outward.x() + n.Y() * fs.outward.y() + n.Z() * fs.outward.z() < 0) f.Reverse();
    return f;
}

Result<TopoDS_Face> makeFace(const FaceSpec& fs, const Handle(Geom_Surface)& surf, const std::vector<TopoDS_Edge>& edges, double tol) {
    // Orientation (§9.6ж item 3): loops go counterclockwise around the outward normal; if the surface normal at point
    // inside is opposite to the outward one, the face is built on the surface with reversed loops and then reversed.
    GeomAPI_ProjectPointOnSurf pr(P(fs.inside), surf);
    if (!pr.IsDone() || pr.NbPoints() == 0) return buildError("face inside");
    double u, v;
    pr.LowerDistanceParameters(u, v);
    GeomLProp_SLProps props(surf, u, v, 1, 1e-9);
    if (!props.IsNormalDefined()) return buildError("face normal");
    const gp_Dir n = props.Normal();
    const bool flip = n.X() * fs.outward.x() + n.Y() * fs.outward.y() + n.Z() * fs.outward.z() < 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool rev = flip != (attempt == 1);
        BRep_Builder b;
        TopoDS_Face f;
        b.MakeFace(f, surf, tol);
        for (const auto& loop : fs.loops) {
            TopoDS_Wire w;
            b.MakeWire(w);
            for (const auto& le : loop) {
                TopoDS_Edge e = edges[size_t(le.edge)];
                b.Add(w, le.reversed ? TopoDS::Edge(e.Reversed()) : e);
            }
            w.Closed(true);
            b.Add(f, rev ? TopoDS::Wire(w.Reversed()) : w);
        }
        ShapeFix_Face fix(f);
        fix.SetPrecision(tol);
        fix.SetMaxTolerance(1e-3);
        fix.FixOrientationMode() = 0;  // loop orientation is set by the description; checked with the point inside
        fix.FixAddNaturalBoundMode() = 0;
        fix.Perform();
        TopoDS_Face g = fix.Face();
        if (insideFace(g, fs.inside, tol)) {
            if (flip) g.Reverse();
            return g;
        }
    }
    return buildError("face domain");
}

}  // namespace

Result<TopoDS_Shape> buildSolidFromFaces(const SolidSpec& spec, const Cancel* cancel) {
    try {
        std::vector<TopoDS_Vertex> vs;
        for (const auto& p : spec.vertices) vs.push_back(BRepBuilderAPI_MakeVertex(P(p)).Vertex());
        std::vector<Handle(Geom_Surface)> surfs;
        for (const auto& s : spec.surfaces) {
            surfs.push_back(makeSurface(s));
            if (surfs.back().IsNull()) return buildError("surface");
        }
        std::vector<TopoDS_Edge> edges;
        for (size_t i = 0; i < spec.edges.size(); ++i) {
            const auto& e = spec.edges[i];
            if (e.v1 < 0 || e.v2 < 0 || size_t(e.v1) >= vs.size() || size_t(e.v2) >= vs.size()) return buildError("edge " + std::to_string(i));
            auto r = makeEdge(e, vs, spec.vertices, surfs, spec.tolerance);
            if (!r) return buildError("edge " + std::to_string(i));
            edges.push_back(*r);
        }
        BRepBuilderAPI_Sewing sew(spec.tolerance);
        for (size_t i = 0; i < spec.faces.size(); ++i) {
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            const auto& fs = spec.faces[i];
            if (fs.surface < 0 || size_t(fs.surface) >= surfs.size()) return buildError("face " + std::to_string(i));
            for (const auto& l : fs.loops)
                for (const auto& le : l)
                    if (le.edge < 0 || size_t(le.edge) >= edges.size()) return buildError("face " + std::to_string(i));
            if (auto cap = capFace(fs, spec, spec.tolerance)) {
                sew.Add(*cap);
                continue;
            }
            auto f = makeFace(fs, surfs[size_t(fs.surface)], edges, spec.tolerance);
            if (!f) return buildError("face " + std::to_string(i) + " " + *f.error().stringParam("detail"));
            sew.Add(*f);
        }
        sew.Perform();
        if (sew.NbFreeEdges() > 0) return buildError("sewing free edges " + std::to_string(sew.NbFreeEdges()));
        const TopoDS_Shape sewn = sew.SewedShape();
        std::vector<TopoDS_Shell> shells;
        for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) shells.push_back(TopoDS::Shell(ex.Current()));
        if (shells.size() != 1) return buildError("shells " + std::to_string(shells.size()));
        BRepBuilderAPI_MakeSolid ms(shells.front());
        if (!ms.IsDone()) return buildError("solid");
        ShapeFix_Solid fs;
        fs.Init(ms.Solid());
        fs.Perform();
        TopoDS_Shape solid = fs.Solid();
        TopExp_Explorer se(solid, TopAbs_SOLID);
        if (!se.More()) return buildError("solid");
        TopoDS_Solid sol = TopoDS::Solid(se.Current());
        BRepLib::OrientClosedSolid(sol);
        ShapeUpgrade_UnifySameDomain unify(sol, true, true, false);
        unify.Build();
        TopoDS_Shape out = unify.Shape();
        TopExp_Explorer oe(out, TopAbs_SOLID);
        if (!oe.More()) return buildError("unify");
        out = oe.Current();
        if (!BRepCheck_Analyzer(out).IsValid()) {
            ShapeFix_Shape sfs(out);
            sfs.SetPrecision(spec.tolerance);
            sfs.SetMaxTolerance(1e-3);
            sfs.Perform();
            out = sfs.Shape();
            TopExp_Explorer fe(out, TopAbs_SOLID);
            if (!fe.More()) return buildError("invalid");
            out = fe.Current();
            if (!BRepCheck_Analyzer(out).IsValid()) return buildError("invalid");
        }
        GProp_GProps g;
        BRepGProp::VolumeProperties(out, g);
        if (!(g.Mass() > 0)) return buildError("volume");
        return out;
    } catch (const Standard_Failure& e) {
        return buildError(std::string("exception ") + e.what());
    }
}

Result<TopoDS_Shape> buildSolidFromMesh(const MeshSpec& m, const Cancel* cancel) {
    try {
        BRep_Builder b;
        std::vector<TopoDS_Vertex> vs;
        for (const auto& p : m.vertices) vs.push_back(BRepBuilderAPI_MakeVertex(P(p)).Vertex());
        std::map<std::pair<int, int>, TopoDS_Edge> edgeOf;  // {min, max} → edge directed from min to max
        TopoDS_Shell shell;
        b.MakeShell(shell);
        for (size_t t = 0; t < m.triangles.size(); ++t) {
            if (t % 1024 == 0 && cancelled(cancel)) return Error("KERNEL_CANCELLED");
            const auto& tri = m.triangles[t];
            for (int k : tri)
                if (k < 0 || size_t(k) >= vs.size()) return buildError("mesh index");
            const Point3 &a = m.vertices[size_t(tri[0])], &bb = m.vertices[size_t(tri[1])], &c = m.vertices[size_t(tri[2])];
            const Vec3 n = (bb - a).cross(c - a);
            if (n.norm() < 1e-14) return buildError("mesh degenerate");
            TopoDS_Wire w;
            b.MakeWire(w);
            for (int k = 0; k < 3; ++k) {
                const int i = tri[size_t(k)], j = tri[size_t((k + 1) % 3)];
                const auto key = std::make_pair(std::min(i, j), std::max(i, j));
                auto it = edgeOf.find(key);
                if (it == edgeOf.end()) {
                    BRepBuilderAPI_MakeEdge mk(vs[size_t(key.first)], vs[size_t(key.second)]);
                    if (!mk.IsDone()) return buildError("mesh edge");
                    it = edgeOf.emplace(key, mk.Edge()).first;
                }
                b.Add(w, i < j ? it->second : TopoDS::Edge(it->second.Reversed()));
            }
            w.Closed(true);
            BRepBuilderAPI_MakeFace mf(gp_Pln(P(a), Dn(n.normalized())), w, true);
            if (!mf.IsDone()) return buildError("mesh face");
            b.Add(shell, mf.Face());
        }
        shell.Closed(BRep_Tool::IsClosed(shell));
        if (!shell.Closed()) return buildError("mesh open");
        TopoDS_Solid sol;
        b.MakeSolid(sol);
        b.Add(sol, shell);
        BRepLib::OrientClosedSolid(sol);
        TopoDS_Shape out = sol;
        // Merging coplanar triangles into polygons (§3.23): for meshes of up to KERNEL_MESH_UNIFY_MAX triangles.
        if (m.triangles.size() <= KERNEL_MESH_UNIFY_MAX) {
            ShapeUpgrade_UnifySameDomain unify(sol, true, true, false);
            unify.SetLinearTolerance(1e-9);
            unify.SetAngularTolerance(1e-9);
            unify.Build();
            TopExp_Explorer ex(unify.Shape(), TopAbs_SOLID);
            if (ex.More() && BRepCheck_Analyzer(ex.Current()).IsValid()) out = ex.Current();
        }
        GProp_GProps g;
        BRepGProp::VolumeProperties(out, g);
        if (!(g.Mass() > 0)) return buildError("volume");
        return out;
    } catch (const Standard_Failure& e) {
        return buildError(std::string("exception ") + e.what());
    }
}

}  // namespace construe::kernel::occt
