// Sheet constructions (spec/drawing.md §6.29, FX14-DRW-006): thin lines drawn on a view — a segment, a circle, an arc,
// a pitch circle through hole centers, an axis line; points are bound to the view model by stable names (as dimension
// references, §6а.2) or free in view coordinates.
#include "drawing/dimension_parts.h"

#include <algorithm>
#include <cmath>

namespace construe::drawing {

namespace dimdetail {

namespace {

constexpr double PI = 3.14159265358979323846;

// Error of a construction point binding (§6.29 «Errors») from the error of its reference (§6а.2).
Error bindError(const Error& e, const std::string& name) {
    const char* code = e.code == "DRAWING_DIM_REF_LOST"        ? "DRAWING_CONS_REF_LOST"
                       : e.code == "DRAWING_DIM_REF_AMBIGUOUS" ? "DRAWING_CONS_REF_AMBIGUOUS"
                                                               : "DRAWING_CONS_BAD_REF";
    return Error(code).with("name", name);
}

// Point of a construction in view coordinates (§6.29 «Points»): the bound model point; a free one — xy.
Result<Point2> pointOf(const Ctx& c, const doc::ConstructionPoint& p) {
    if (p.bind.empty()) return Point2(p.xy[0], p.xy[1]);
    doc::DimensionRef ref;
    ref.kind = p.bind;
    ref.name = p.name;
    auto g = refGeom(c, ref);
    if (!g) return bindError(g.error(), p.name);
    if (p.bind == "vertex" || p.bind == "center") {
        if (g->kind != RefGeom::Kind::Point) return Error("DRAWING_CONS_BAD_REF").with("name", p.name);
        return g->a;
    }
    switch (g->kind) {  // edge
        case RefGeom::Kind::Segment: return p.at ? Point2(g->a + *p.at * (g->b - g->a)) : Point2((g->a + g->b) / 2);
        case RefGeom::Kind::Circle:
        case RefGeom::Kind::Arc:
            if (p.at) break;  // a point on a circle — not supported
            return g->center;
        default: break;
    }
    return Error("DRAWING_CONS_BAD_REF").with("name", p.name);
}

// Circle through n ≥ 3 points (§6.29 «Shape»): least squares of the algebraic distance in coordinates centered at the
// centroid (exact for points on one circle); radius — the mean distance from the center; nullopt — collinear.
std::optional<std::pair<Point2, double>> fitCircle(const std::vector<Point2>& pts) {
    const double n = double(pts.size());
    Point2 m(0, 0);
    for (const auto& p : pts) m += p;
    m /= n;
    double suu = 0, svv = 0, suv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0;
    for (const auto& p : pts) {
        const double u = p.x() - m.x(), v = p.y() - m.y();
        suu += u * u, svv += v * v, suv += u * v;
        suuu += u * u * u, svvv += v * v * v, suvv += u * v * v, svuu += v * u * u;
    }
    const double det = suu * svv - suv * suv;
    if (!(std::abs(det) > CONSTRUCTION_COLLINEAR_TOL * (suu + svv) * (suu + svv))) return std::nullopt;
    const double bu = 0.5 * (suuu + suvv), bv = 0.5 * (svvv + svuu);
    const Point2 c(m.x() + (bu * svv - bv * suv) / det, m.y() + (suu * bv - suv * bu) / det);
    double r = 0;
    for (const auto& p : pts) r += (p - c).norm();
    return std::pair{c, r / n};
}

}  // namespace

Result<ConstructionShape> constructionShape(const Ctx& c, const doc::SheetConstruction& cs) {
    ConstructionShape s;
    for (const auto& p : cs.points) {
        auto q = pointOf(c, p);
        if (!q) {
            if (!s.bindError) s.bindError = q.error();
            s.points.emplace_back(p.xy[0], p.xy[1]);  // the stored position (§6.29 «Errors»)
        } else {
            s.points.push_back(*q);
        }
    }
    const Error degenerate = Error("DRAWING_CONS_DEGENERATE").with("id", cs.id);
    const auto& P = s.points;
    RefGeom& g = s.geom;
    g.depth = g.depthMin = 0;
    if (cs.kind == "line" || cs.kind == "axis") {
        if (P.size() != 2 || (P[1] - P[0]).norm() < DRAWING_MIN_LINE) return degenerate;
        g.kind = RefGeom::Kind::Segment;
        g.a = P[0], g.b = P[1];
        g.p1 = Point3(P[0].x() * c.vp.r + P[0].y() * c.vp.u), g.p2 = Point3(P[1].x() * c.vp.r + P[1].y() * c.vp.u);
        g.dir3 = (g.p2 - g.p1).normalized();
    } else if (cs.kind == "circle") {
        if (P.empty()) return degenerate;
        const double r = cs.radius ? *cs.radius : P.size() > 1 ? (P[1] - P[0]).norm() : 0.0;
        if (!(r >= DRAWING_MIN_LINE)) return degenerate;
        g.kind = RefGeom::Kind::Circle;
        g.center = P[0], g.radius = r;
    } else if (cs.kind == "arc") {
        if (P.size() != 3) return degenerate;
        const Point2 v1 = P[1] - P[0], v2 = P[2] - P[0];
        if (v1.norm() < DRAWING_MIN_LINE || v2.norm() < DRAWING_MIN_LINE) return degenerate;
        g.kind = RefGeom::Kind::Arc;
        g.center = P[0], g.radius = v1.norm();
        g.start = std::atan2(v1.y(), v1.x()), g.end = std::atan2(v2.y(), v2.x());
        if (g.end <= g.start) g.end += 2 * PI;
        if (g.end - g.start < 1e-9) return degenerate;
    } else if (cs.kind == "pitchCircle") {
        if (P.size() < 3) return degenerate;
        const auto f = fitCircle(P);
        if (!f || f->second < DRAWING_MIN_LINE) return degenerate;
        g.kind = RefGeom::Kind::Circle;
        g.center = f->first, g.radius = f->second;
    } else {
        return Error("API_INVALID_DEFINITION").with("detail", std::string("kind"));
    }
    return s;
}

Result<RefGeom> constructionRefGeom(const Ctx& c, const std::string& id) {
    const int ci = c.sheet ? c.sheet->constructionIndex(id) : -1;
    if (ci < 0) return Error("DRAWING_DIM_REF_LOST").with("name", id);
    const doc::SheetConstruction& cs = c.sheet->constructions[size_t(ci)];
    if (cs.view != c.viewId) return badRefs("refs");
    auto s = constructionShape(c, cs);
    if (!s) return badRefs("zero");
    return s->geom;
}

}  // namespace dimdetail

namespace {

using dimdetail::RefGeom;

LineType constructionLineType(const doc::SheetConstruction& cs) {
    const std::string& l = cs.line.empty() ? defaultConstructionLine(cs.kind) : cs.line;
    return l == "dashDot" ? LineType::DashDot : l == "dashed" ? LineType::Dashed : LineType::Solid;
}

// Primitives of a construction (§6.29 «Drawing»): thin, role cons:<id>, sheet coordinates.
std::vector<Primitive> constructionPrims(const dimdetail::Ctx& c, const doc::SheetConstruction& cs, const RefGeom& g) {
    Primitive p;
    p.weight = Weight::Thin;
    p.lineType = constructionLineType(cs);
    p.role = "cons:" + cs.id;
    if (g.kind == RefGeom::Kind::Segment) {
        p.kind = PrimitiveKind::Line;
        Point2 a = c.T(g.a), b = c.T(g.b);
        if (cs.kind == "axis") {  // the axis line runs past its points (GOST 2.307: by 2…5 mm)
            const Point2 d = (b - a).normalized();
            a -= CONSTRUCTION_AXIS_OVERSHOOT_MM * d, b += CONSTRUCTION_AXIS_OVERSHOOT_MM * d;
        }
        p.points = {a, b};
    } else {
        p.kind = g.kind == RefGeom::Kind::Arc ? PrimitiveKind::Arc : PrimitiveKind::Circle;
        p.center = c.T(g.center);
        p.radius = g.radius * c.vp.scale;
        p.start = g.start, p.end = g.end;
    }
    return {p};
}

}  // namespace

const std::string& defaultConstructionLine(const std::string& kind) {
    static const std::string solid = "solid", dashDot = "dashDot";
    return kind == "pitchCircle" || kind == "axis" ? dashDot : solid;
}

Result<ConstructionPreview> previewConstruction(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                                const doc::SheetConstruction& cs, const ModelProvider& models, const SheetGraphics* built) {
    bool viewError = false;
    auto su = dimdetail::setup(sheet, cs.view, models, viewError, built);
    if (!su) {
        if (su.error().code == "DRAWING_DIM_VIEW_NOT_FOUND") return Error("DRAWING_CONS_VIEW_NOT_FOUND").with("view", cs.view);
        return su.error();
    }
    const dimdetail::Ctx c = dimdetail::makeCtx(d, &drawing, sheet, cs.view, *su, models.kernel(), nullptr);
    auto s = dimdetail::constructionShape(c, cs);
    if (!s) return s.error();
    ConstructionPreview out;
    out.construction = cs;
    out.primitives = constructionPrims(c, cs, s->geom);
    for (const auto& q : s->points) out.points.push_back(c.T(q));
    out.error = s->bindError;
    return out;
}

void appendConstructions(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                         SheetGraphics& out) {
    for (const auto& cs : sheet.constructions) {
        // The view already built (§4): not computed yet — the construction is pending too; with an error — no own error.
        if (std::find(out.pending.begin(), out.pending.end(), cs.view) != out.pending.end()) {
            out.pending.push_back(cs.id);
            continue;
        }
        if (std::any_of(out.errors.begin(), out.errors.end(), [&](const ViewError& e) { return e.view == cs.view; })) continue;
        auto r = previewConstruction(d, drawing, sheet, cs, models, &out);
        if (!r) {
            if (r.error().code == VIEW_PENDING) out.pending.push_back(cs.id);
            else out.errors.push_back({cs.id, r.error()});
            continue;
        }
        for (auto& p : r->primitives) out.primitives.push_back(std::move(p));
        if (r->error) out.errors.push_back({cs.id, *r->error});  // drawn by the stored points, marked with the error
    }
}

}  // namespace construe::drawing
