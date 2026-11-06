// Gear rim (spec/regen.md §4.17; profile — spec/gear.md).
#include "regen/eval_common.h"

#include "gear/gear.h"
#include "sheet/sheet.h"

#include <cmath>

namespace construe::regen {

using namespace detail;

namespace {

// Outline segment of the gear plane → kernel profile segment (gear.md §2.4, kernel-interface.md §2.5).
kernel::Segment3 toKernel(const gear::Seg& s, const Plane& pl) {
    kernel::Segment3 k;
    k.id = s.id;
    switch (s.kind) {
        case gear::Seg::Kind::Line:
            k.kind = kernel::Segment3::Kind::Line;
            k.p1 = pl.toWorld(s.a), k.p2 = pl.toWorld(s.b);
            break;
        case gear::Seg::Kind::Arc:
            k.kind = kernel::Segment3::Kind::Arc;
            k.center = pl.toWorld(s.center), k.radius = s.radius;
            k.start = pl.toWorld(s.ccw ? s.a : s.b), k.end = pl.toWorld(s.ccw ? s.b : s.a);
            k.reversed = !s.ccw;  // traversed clockwise: against the natural direction
            break;
        case gear::Seg::Kind::Circle:
            k.kind = kernel::Segment3::Kind::Circle;
            k.center = pl.toWorld(s.center), k.radius = s.radius;
            break;
        default:
            k.kind = kernel::Segment3::Kind::Spline;
            for (const auto& p : s.curve.poles) k.poles.push_back(pl.toWorld(p));
            k.weights = s.curve.weights, k.knots = s.curve.knots, k.mults = s.curve.mults, k.degree = s.curve.degree;
            break;
    }
    return k;
}

kernel::Loop3 toKernel(const gear::Loop& l, const Plane& pl) {
    kernel::Loop3 out;
    for (const auto& s : l) out.segments.push_back(toKernel(s, pl));
    return out;
}

gear::Seg circleSeg(const std::string& id, double r) {
    gear::Seg c;
    c.kind = gear::Seg::Kind::Circle;
    c.id = id;
    c.radius = r;
    c.a = c.b = Point2(r, 0);
    return c;
}

// Fill of the spaces up to the tip circle (§4.17 item 8): annulus (sector of an annulus) between radii r0 < r1.
gear::Section fillSection(double r0, double r1, bool sector, double t0, double t1) {
    gear::Section s;
    if (!sector) {
        s.outer = {circleSeg("w1", r1)};
        s.holes.push_back({circleSeg("w2", r0)});
        return s;
    }
    auto P = [](double r, double a) { return Point2(r * std::cos(a), r * std::sin(a)); };
    gear::Seg outer, line1, inner, line2;
    outer.kind = gear::Seg::Kind::Arc, outer.id = "w1", outer.radius = r1, outer.a = P(r1, t0), outer.b = P(r1, t1), outer.ccw = true;
    line1.kind = gear::Seg::Kind::Line, line1.id = "w2", line1.a = P(r1, t1), line1.b = P(r0, t1);
    inner.kind = gear::Seg::Kind::Arc, inner.id = "w3", inner.radius = r0, inner.a = P(r0, t1), inner.b = P(r0, t0), inner.ccw = false;
    line2.kind = gear::Seg::Kind::Line, line2.id = "w4", line2.a = P(r0, t0), line2.b = P(r1, t0);
    s.outer = {outer, line1, inner, line2};
    return s;
}

kernel::Profile toProfile(const gear::Section& s, const Plane& pl) {
    kernel::Profile p;
    p.plane = pl;
    p.outer = toKernel(s.outer, pl);
    for (const auto& h : s.holes) p.holes.push_back(toKernel(h, pl));
    return p;
}

}  // namespace

EvalPtr Regenerator::evalGear(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    const doc::Feature& f = c.f;
    const doc::Json& d = f.pattern()->def;
    // 1. Face: cylinder, external (convex) or internal (concave) — as the thread (§4.14 items 1, 3).
    const auto fr = resolveOne(c, *input, d["face"].get<std::string>(), naming::EntityKind::Face, "/face");
    if (!fr) return finish(c, input);
    const auto& fe = input->names.faces.at(fr->index);
    const kernel::Shape& sh = input->bodies.at(fe.body).shape;
    const auto si = k_.faceSurface(sh, fe.face);
    if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) {
        c.error(refGeometry("face", "face not cylindrical"), "/face");
        return finish(c, input);
    }
    const Vec3 a = si.dir.normalized();
    const Vec3 u = a.unitOrthogonal();
    const double rFace = si.radius;
    const bool internal = k_.faceNormalAt(sh, fe.face, si.point + rFace * u).dot(u) < 0;
    // 2. Parameters.
    const auto m = c.field(d["module"].get<std::string>(), expr::LENGTH, "/module");
    if (!m) return finish(c, input);
    const auto zv = c.field(d["teeth"].get<std::string>(), expr::DIMLESS, "/teeth");
    if (!zv) return finish(c, input);
    double x = 0;
    if (d.contains("shift")) {
        const auto xv = c.field(d["shift"].get<std::string>(), expr::DIMLESS, "/shift");
        if (!xv) return finish(c, input);
        x = *xv;
    }
    if (std::abs(*zv - std::round(*zv)) > 1e-9 || std::abs(*zv) > 1e6) {
        c.error(Error("REGEN_BAD_VALUE").with("name", std::string("teeth")).with("actual", *zv), "/teeth");
        return finish(c, input);
    }
    const gear::Params gp{*m, int(std::lround(*zv)), x, internal};
    auto geo = gear::geometry(gp);
    if (!geo) {
        Error e = geo.error();
        if (e.code == "GEAR_BAD_VALUE") {
            const std::string name = *e.stringParam("name");
            const double actual = name == "module" ? *m : name == "teeth" ? double(gp.z) : x;
            c.error(Error("REGEN_BAD_VALUE").with("name", name).with("actual", actual), "/" + name);
        } else {
            c.error(e, "/teeth");
        }
        return finish(c, input);
    }
    // 3. Face diameter against the tip diameter (cut) or the root diameter (add: the teeth grow from the face, FX7-PT-006).
    const bool add = d.value("operation", std::string("cut")) == "add";
    const double D = 2 * rFace;
    const bool smaller = D < geo->da - GEAR_DIAMETER_TOL, larger = D > geo->da + GEAR_DIAMETER_TOL;
    if (add) {
        if (internal ? D < geo->df - GEAR_DIAMETER_TOL : D > geo->df + GEAR_DIAMETER_TOL) {
            c.error(Error("REGEN_GEAR_ROOT_DIAMETER").with("actual", D).with("expected", geo->df), "/face");
            return finish(c, input);
        }
    } else if (internal ? larger : smaller) {
        c.error(Error("REGEN_GEAR_DIAMETER").with("actual", D).with("expected", geo->da), "/face");
        return finish(c, input);
    }
    if (!add && (internal ? smaller : larger))
        c.warning(Error("REGEN_GEAR_DIAMETER_TRIMMED").with("actual", D).with("expected", geo->da));
    for (const auto& w : geo->warnings) c.warning(w);
    // 4. Axial extent and width.
    const auto range = kernel::axialRange(k_, sh, fe.face, si.point, a);
    if (!range) {
        c.error(refGeometry("face", "face not cylindrical"), "/face");
        return finish(c, input);
    }
    double width = range->second - range->first;
    if (d.contains("width")) {
        const auto v = c.field(d["width"].get<std::string>(), expr::LENGTH, "/width");
        if (!v) return finish(c, input);
        if (*v <= 0 || *v > width + SWEEP_JOIN_TOL) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("width")).with("actual", *v), "/width");
            return finish(c, input);
        }
        width = std::min(*v, width);
    }
    const bool rev = d.contains("reverse") && d["reverse"].get<bool>();
    const Vec3 dir = rev ? Vec3(-a) : a;
    // 5. Gear plane: origin on the axis at the rim start, normal along the width, angles from u counterclockwise about dir.
    Plane pl;
    pl.origin = si.point + (rev ? range->second : range->first) * a;
    pl.normal = dir;
    pl.x = u;
    pl.y = dir.cross(u);
    // 6. Layout: full gear with phase, or a sector by angles or by two planar faces.
    gear::Layout lay;
    if (d.contains("phase")) {
        const auto v = c.field(d["phase"].get<std::string>(), expr::ANGLE, "/phase");
        if (!v) return finish(c, input);
        lay.phase = *v;
    }
    if (d.contains("sector")) {
        const doc::Json& sd = d["sector"];
        lay.sector = true;
        if (sd.contains("faces")) {
            double ang[2] = {0, 0};
            for (int i = 0; i < 2; ++i) {
                const std::string sub = "/sector/faces/" + std::to_string(i);
                const auto r = resolveOne(c, *input, sd["faces"][size_t(i)].get<std::string>(), naming::EntityKind::Face, sub);
                if (!r) return finish(c, input);
                const auto& e = input->names.faces.at(r->index);
                const kernel::Shape& bs = input->bodies.at(e.body).shape;
                Vec3 w = k_.faceCentroid(bs, e.face) - pl.origin;
                w -= w.dot(dir) * dir;
                if (k_.faceSurface(bs, e.face).kind != kernel::SurfaceInfo::Kind::Plane || w.norm() < REF_DEGENERATE_TOL) {
                    c.error(refGeometry("sector.faces", "face not planar"), sub);
                    return finish(c, input);
                }
                ang[i] = std::atan2(w.dot(pl.y), w.dot(pl.x));
            }
            lay.start = ang[0];
            lay.angle = std::fmod(ang[1] - ang[0] + 4 * PI, 2 * PI);
        } else {
            const auto s0 = c.field(sd["start"].get<std::string>(), expr::ANGLE, "/sector/start");
            if (!s0) return finish(c, input);
            const auto s1 = c.field(sd["angle"].get<std::string>(), expr::ANGLE, "/sector/angle");
            if (!s1) return finish(c, input);
            lay.start = *s0, lay.angle = *s1;
        }
        if (!(lay.angle > 0) || lay.angle >= 2 * PI - gear::GEAR_ANGLE_TOL) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("sector")).with("actual", lay.angle), "/sector");
            return finish(c, input);
        }
    }
    // 7. Tool: section between the outline and the auxiliary circle, extruded over the width; cut.
    const double ra = geo->da / 2;
    const double aux = internal ? 0.5 * std::min(ra, rFace) : std::max(ra, rFace) + *m;
    // The face carries the tips (D = da): the tool tips go past it, off the blank surface (gear.md §4.5).
    const double tipShift = add || smaller || larger ? 0.0 : GEAR_TIP_SHIFT * *m;
    auto sec = gear::toolSection(gp, lay, aux, tipShift);
    if (!sec) {
        c.error(sec.error(), lay.sector ? "/sector" : "/teeth");
        return finish(c, input);
    }
    kernel::Extent ext;
    ext.kind = kernel::Extent::Kind::Blind;
    ext.depth = width;
    // A rim end at the end of the body (its box along the axis) — the tool goes past it by m: no coplanar caps.
    Plane toolPlane = pl;
    kernel::Extent toolExt = ext;
    {
        const BBox3 bb = k_.bbox(sh);
        double lo = 1e300, hi = -1e300;
        for (int i = 0; i < 8; ++i) {
            const double t = (bb.corner(static_cast<BBox3::CornerType>(i)) - si.point).dot(a);
            lo = std::min(lo, t), hi = std::max(hi, t);
        }
        const double r0 = rev ? range->second - width : range->first, r1 = rev ? range->second : range->first + width;
        const bool freeLow = r0 <= lo + SWEEP_JOIN_TOL, freeHigh = r1 >= hi - SWEEP_JOIN_TOL;
        const double eStart = (rev ? freeHigh : freeLow) ? *m : 0.0, eEnd = (rev ? freeLow : freeHigh) ? *m : 0.0;
        toolPlane.origin = pl.origin - eStart * dir;
        toolExt.depth = width + eStart + eEnd;
    }
    // 8. Record for the drawing (drawing.md §6.20): the fill of the spaces up to the tip circle.
    auto record = [&] {
        GearEntry g;
        g.feature = f.id;
        g.internal = internal;
        g.sector = lay.sector;
        g.module = *m, g.shift = x, g.teeth = gp.z, g.formed = sec->teeth;
        g.accuracy = d.value("accuracy", std::string());
        g.origin = pl.origin, g.dir = dir, g.ref = u, g.width = width;
        const double tau = 2 * PI / gp.z;
        g.start = lay.sector ? lay.start : 0;
        g.angle = lay.sector ? sec->teeth * tau : 0;
        g.d = geo->d, g.da = geo->da, g.df = geo->df;
        const double rf = geo->df / 2, dr = GEAR_FILL_DEPTH * *m;
        const gear::Section fs = internal ? fillSection(ra, rf + dr, g.sector, g.start, g.start + g.angle)
                                          : fillSection(rf - dr, ra, g.sector, g.start, g.start + g.angle);
        if (auto fr2 = k_.extrude(toProfile(fs, pl), ext, false)) g.fill = fr2->shape;
        st->gears.push_back(std::move(g));
    };
    // Teeth ring of «Add»: from the face (overlapping it by GEAR_ADD_OVERLAP·m) to the tips (FX7-PT-006).
    const double addOverlap = GEAR_ADD_OVERLAP * *m;
    const gear::Section ring = internal ? fillSection(ra, rFace + addOverlap, lay.sector, lay.start, lay.start + lay.angle)
                                        : fillSection(rFace - addOverlap, ra, lay.sector, lay.start, lay.start + lay.angle);
    // 7а. Sheet metal part (§4.16 item 5а, sheet.md §4.6в, FX7-SH-004): a rim on a wall end goes into the sheet model.
    if (input->sheet && input->sheet->model && input->bodies.at(fe.body).id == input->sheet->body &&
        input->bodies.at(fe.body).shape == input->sheet->folded) {
        sheet::Model sm = *input->sheet->model;
        const std::string owner = fe.base.substr(0, fe.base.find('.'));
        const auto wall = sheet::wallOfRim(sm, a, k_.closestPointOnFace(sh, fe.face, k_.faceCentroid(sh, fe.face)));
        if (!wall) {
            c.error(Error("SHEET_GEAR_PLANE").with("id", owner).with("name", fe.full), "/face");
            return finish(c, input);
        }
        const Transform3 toFlat = sheet::fold(sm, *wall).inverse();
        const double T = sm.params.thickness;
        const double z0 = (toFlat * pl.origin).z(), z1 = (toFlat * Point3(pl.origin + width * dir)).z();
        if (std::abs(std::min(z0, z1)) > sheet::SHEET_MATCH_TOL || std::abs(std::max(z0, z1) - T) > sheet::SHEET_MATCH_TOL) {
            c.error(Error("SHEET_GEAR_THICKNESS").with("id", owner).with("name", fe.full).with("actual", width).with("expected", T),
                    d.contains("width") ? "/width" : "/face");
            return finish(c, input);
        }
        const auto label = [&](const std::string& id) { return f.id + ".side(" + id + ")"; };
        auto toolRegion = sheet::regionFromProfile(toProfile(*sec, pl), toFlat, label);
        auto ringRegion = sheet::regionFromProfile(toProfile(ring, pl), toFlat, label);
        if (!toolRegion || !ringRegion) {
            c.error(Error("SHEET_PROFILE_INVALID").with("detail", std::string("ellipse")), "/face");
            return finish(c, input);
        }
        if (add) sm.pieces[size_t(*wall)].ops.push_back({sheet::PieceOp::Kind::Add, std::move(*ringRegion), {std::move(*toolRegion)}});
        else sheet::addCut(sm, std::move(*toolRegion), f.id, *wall);
        record();
        return sheetRebuild(c, input, st, cancelledOut, std::move(sm));
    }
    if (add) {
        // «Add»: the cut tool goes past both ends of the rim, the teeth stay within the width.
        toolPlane.origin = pl.origin - *m * dir;
        toolExt.depth = width + 2 * *m;
    }
    auto r = k_.extrude(toProfile(*sec, toolPlane), toolExt, false);
    if (!r) {
        c.kernelError(r.error());
        return finish(c, input);
    }
    c.kernelWarnings(r->warnings);
    ToolBuild tb;
    std::vector<Error> nw;
    tb.bases = naming::assignBases(naming::OpKind::Generate, f.id, *r, {}, &nw);
    for (auto& w : nw) c.warning(w);
    tb.shape = r->shape;
    tb.ok = true;
    if (add) {
        // Teeth = the ring minus the cut tool (FX7-PT-006).
        auto rr = k_.extrude(toProfile(ring, pl), ext, false);
        if (!rr) {
            c.kernelError(rr.error());
            return finish(c, input);
        }
        const auto ringBases = naming::assignBases(naming::OpKind::Generate, f.id, *rr, {}, &nw);
        auto teeth = k_.boolean(kernel::BoolKind::Subtract, rr->shape, tb.shape, c.cancel);
        if (!teeth) {
            c.kernelError(teeth.error());
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input);
        }
        c.kernelWarnings(teeth->warnings);
        tb.bases = naming::assignBases(naming::OpKind::Boolean, f.id, *teeth, {inputOf(ringBases), inputOf(tb.bases)}, &nw);
        for (auto& w : nw) c.warning(w);
        tb.shape = teeth->shape;
    }
    if (!applyOperation(c, *st, add ? "add" : "cut", tb)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, tb.shape);
    }
    if (!c.hasError()) {
        record();
        st->tools[f.id] = {tb.shape, tb.bases, add ? "add" : "cut"};  // a source of patterns and mirrors (FX7-PT-008)
    }
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, tb.shape);
}

}  // namespace construe::regen
