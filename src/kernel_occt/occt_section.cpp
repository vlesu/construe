// Sectioning bodies by a plane on OCCT (spec/kernel-interface.md §3.18, §9.9в).
#include "kernel_occt/occt_hlr.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <NCollection_List.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <string>

namespace construe::kernel::occt {

namespace {

constexpr double ON_PLANE_TOL = 1e-6;  // mm: the face lies in the cutting plane (§9.9в, fallback path)
constexpr double SAME_POINT = 1e-9;    // mm: coincident adjacent loop points

double signedArea(const std::vector<Point2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const Point2& u = p[i];
        const Point2& v = p[(i + 1) % p.size()];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}

// Face wire → polyline in view coordinates (§3.18).
std::vector<Point2> wireLoop(const TopoDS_Wire& w, const TopoDS_Face& f, const Vec3& right, const Vec3& up, double chordTol) {
    std::vector<Point2> out;
    auto add = [&](const gp_Pnt& p) {
        const Point2 q(p.X() * right.x() + p.Y() * right.y() + p.Z() * right.z(), p.X() * up.x() + p.Y() * up.y() + p.Z() * up.z());
        if (out.empty() || (out.back() - q).norm() > SAME_POINT) out.push_back(q);
    };
    for (BRepTools_WireExplorer ex(w, f); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = ex.Current();
        if (BRep_Tool::Degenerated(e)) continue;
        BRepAdaptor_Curve c(e);
        const double a = c.FirstParameter(), b = c.LastParameter();
        std::vector<gp_Pnt> pts;
        if (c.GetType() == GeomAbs_Line) {
            pts = {c.Value(a), c.Value(b)};
        } else {
            GCPnts_QuasiUniformDeflection d(c, chordTol, a, b);
            if (d.IsDone() && d.NbPoints() >= 2)
                for (int i = 1; i <= d.NbPoints(); ++i) pts.push_back(d.Value(i));
            else
                pts = {c.Value(a), c.Value(b)};
            pts.front() = c.Value(a);  // loop vertices are the exact edge ends
            pts.back() = c.Value(b);
        }
        if (e.Orientation() == TopAbs_REVERSED) std::reverse(pts.begin(), pts.end());
        for (const auto& p : pts) add(p);
    }
    if (out.size() > 1 && (out.front() - out.back()).norm() <= SAME_POINT) out.pop_back();
    return out;
}

}  // namespace

Result<OcctSectionCut> sectionCutOcct(const std::vector<std::pair<TopoDS_Shape, gp_Trsf>>& items, const ViewDirection& view,
                                      const Point3& origin, double chordTol, const Cancel* cancel) {
    ensureSharedStatics();
    OcctSectionCut out;
    const Vec3 n = view.toViewer, up = view.up, right = up.cross(n);
    const gp_Dir N(n.x(), n.y(), n.z()), Rt(right.x(), right.y(), right.z());
    const gp_Pnt O(origin.x(), origin.y(), origin.z());
    auto side = [&](const gp_Pnt& p) { return (p.X() - O.X()) * n.x() + (p.Y() - O.Y()) * n.y() + (p.Z() - O.Z()) * n.z(); };
    for (size_t it = 0; it < items.size(); ++it) {
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        try {
            const TopoDS_Shape world = BRepBuilderAPI_Transform(items[it].first, items[it].second, /*copy*/ true).Shape();
            Bnd_Box box;
            BRepBndLib::Add(world, box);
            if (box.IsVoid()) continue;
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            double smin = 1e300, smax = -1e300;
            for (int c = 0; c < 8; ++c) {
                const double s = side(gp_Pnt(c & 1 ? x1 : x0, c & 2 ? y1 : y0, c & 4 ? z1 : z0));
                smin = std::min(smin, s);
                smax = std::max(smax, s);
            }
            if (smax <= 0) {  // entirely on the remaining side
                out.bodies.push_back({int(it), world});
                continue;
            }
            if (smin > 0) continue;  // removed entirely
            const gp_Pnt mid((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
            const double L = 2 * std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0)) + 10;
            const double sm = side(mid);
            const gp_Pnt o(mid.X() - sm * n.x(), mid.Y() - sm * n.y(), mid.Z() - sm * n.z());  // center projected onto the plane
            const gp_Pnt corner(o.X() - L * right.x() - L * up.x(), o.Y() - L * right.y() - L * up.y(), o.Z() - L * right.z() - L * up.z());
            BRepPrimAPI_MakeBox mkBox(gp_Ax2(corner, N, Rt), 2 * L, 2 * L, L);
            const TopoDS_Shape tool = mkBox.Shape();
            const TopoDS_Face bottom = mkBox.BottomFace();
            TopoDS_Shape res;
            std::vector<TopoDS_Face> caps;
            bool done = false;
            for (int attempt = 0; attempt < 2 && !done; ++attempt) {
                BRepAlgoAPI_Cut op;
                NCollection_List<TopoDS_Shape> args, tools;
                args.Append(world);
                tools.Append(tool);
                op.SetArguments(args);
                op.SetTools(tools);
                op.SetRunParallel(false);
                op.SetNonDestructive(true);
                op.SetToFillHistory(true);
                op.SetFuzzyValue(attempt == 0 ? 0.0 : KERNEL_BOOLEAN_FUZZY);
                op.Build();
                if (op.HasErrors() || !op.IsDone()) continue;
                res = op.Shape();
                caps.clear();
                for (const TopoDS_Shape& m : op.Modified(bottom))
                    if (m.ShapeType() == TopAbs_FACE) caps.push_back(TopoDS::Face(m));
                done = true;
            }
            if (!done) return Error("KERNEL_SECTION_FAILED").with("detail", std::string("cut"));
            if (caps.empty()) {  // fallback path: planar result faces in the cutting plane
                for (TopExp_Explorer ex(res, TopAbs_FACE); ex.More(); ex.Next()) {
                    const TopoDS_Face& f = TopoDS::Face(ex.Current());
                    BRepAdaptor_Surface s(f);
                    if (s.GetType() != GeomAbs_Plane) continue;
                    const gp_Pln pl = s.Plane();
                    if (std::abs(std::abs(pl.Axis().Direction().Dot(N)) - 1) > 1e-9) continue;
                    if (std::abs(side(pl.Location())) > ON_PLANE_TOL) continue;
                    caps.push_back(f);
                }
            }
            bool any = false;
            for (TopExp_Explorer ex(res, TopAbs_SOLID); ex.More(); ex.Next()) any = true;
            if (any) out.bodies.push_back({int(it), res});
            for (const TopoDS_Face& f : caps) {
                SectionRegion r;
                r.item = int(it);
                const TopoDS_Wire outer = BRepTools::OuterWire(f);
                if (!outer.IsNull()) r.loops.push_back(wireLoop(outer, f, right, up, chordTol));
                for (TopExp_Explorer ex(f, TopAbs_WIRE); ex.More(); ex.Next()) {
                    const TopoDS_Wire& w = TopoDS::Wire(ex.Current());
                    if (!outer.IsNull() && w.IsSame(outer)) continue;
                    r.loops.push_back(wireLoop(w, f, right, up, chordTol));
                }
                std::erase_if(r.loops, [](const std::vector<Point2>& l) { return l.size() < 3; });
                if (r.loops.empty()) continue;
                for (size_t i = 0; i < r.loops.size(); ++i) {
                    const double a = signedArea(r.loops[i]);
                    if ((i == 0) != (a > 0)) std::reverse(r.loops[i].begin(), r.loops[i].end());
                }
                if (std::abs(signedArea(r.loops[0])) <= 0) continue;
                out.regions.push_back(std::move(r));
            }
        } catch (const Standard_Failure& e) {
            return Error("KERNEL_SECTION_FAILED").with("detail", std::string(e.what() ? e.what() : "Standard_Failure"));
        }
    }
    return out;
}

}  // namespace construe::kernel::occt
