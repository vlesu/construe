// Drawing views (spec/drawing.md §6): main, projected, sections and cross-sections, detail views.
#include "drawing/dimension_parts.h"
#include "drawing/render.h"
#include "drawing/view_parts.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

namespace construe::drawing {

using detail::Cut;
using detail::Frame;

namespace {

std::optional<Frame> baseFrame(const std::string& o) {
    auto mk = [](Vec3 n, Vec3 u) { return Frame{n, u, u.cross(n)}; };
    if (o == "front") return mk(Vec3::UnitZ(), Vec3::UnitY());
    if (o == "back") return mk(-Vec3::UnitZ(), Vec3::UnitY());
    if (o == "top") return mk(Vec3::UnitY(), -Vec3::UnitZ());
    if (o == "bottom") return mk(-Vec3::UnitY(), Vec3::UnitZ());
    if (o == "left") return mk(-Vec3::UnitX(), Vec3::UnitY());
    if (o == "right") return mk(Vec3::UnitX(), Vec3::UnitY());
    if (o == "iso") return mk(Vec3(1, 1, 1).normalized(), Vec3(-1, 2, -1).normalized());
    return std::nullopt;
}

// Projected view from the parent (§6.2).
std::optional<Frame> projectedFrame(const Frame& p, const std::string& dir) {
    if (dir == "top") return Frame{p.u, -p.n, p.r};
    if (dir == "bottom") return Frame{-p.u, p.n, p.r};
    if (dir == "left") return Frame{-p.r, p.u, p.n};
    if (dir == "right") return Frame{p.r, p.u, -p.n};
    return std::nullopt;
}

// Section line (§6.11): endpoints, direction d, view direction s (taking flip into account); nullopt — degenerate.
struct SectionLine {
    Point2 p1, p2, d, s;
};
std::optional<SectionLine> sectionLine(const doc::DrawingView& v) {
    const Point2 p1(v.line[0][0], v.line[0][1]), p2(v.line[1][0], v.line[1][1]);
    const double L = (p2 - p1).norm();
    if (!(L >= DRAWING_MIN_LINE)) return std::nullopt;
    const Point2 d = (p2 - p1) / L;
    Point2 s(d.y(), -d.x());
    if (v.flip) s = -s;
    return SectionLine{p1, p2, d, s};
}

// Section line bound to parent elements by stable names (§6.11 «Line binding», FX9-DRW-002): the line in parent
// coordinates computed from the parent model; nullopt — the model is not computed yet (the stored line is used);
// an error — a reference is lost or the line degenerates (the stored line and DRAWING_SECTION_BIND_LOST).
Result<std::optional<std::array<Point2, 2>>> boundSectionLine(const doc::DrawingView& v, const doc::DrawingView& parentBase, const Frame& pf,
                                                              double parentScale, const ModelProvider& models) {
    using Line = std::optional<std::array<Point2, 2>>;
    const doc::Json& b = *v.lineBind;
    auto st = viewModel(models, parentBase);
    if (!st) {
        if (st.error().code == VIEW_PENDING) return Line();
        return st.error();
    }
    if (!*st) return Line();
    ViewMapping m;
    m.base = &parentBase;
    m.n = pf.n, m.u = pf.u, m.r = pf.r;
    const dimdetail::Ctx c{m, **st, models.kernel()};
    const Error lost = Error("DRAWING_SECTION_BIND_LOST").with("id", v.id);
    const Point2 s1(v.line[0][0], v.line[0][1]), s2(v.line[1][0], v.line[1][1]);
    if (b.contains("axis")) {
        auto g = dimdetail::refGeom(c, doc::DimensionRef{"axis", b["axis"].get<std::string>()});
        if (!g || g->kind != dimdetail::RefGeom::Kind::Segment) return lost;
        const Point2 dir = (g->b - g->a).normalized();
        const double e = SECTION_BIND_OVERSHOOT_MM / parentScale;
        std::array<Point2, 2> l{Point2(g->a - e * dir), Point2(g->b + e * dir)};
        if ((l[1] - l[0]).dot(s2 - s1) < 0) std::swap(l[0], l[1]);
        return Line(l);
    }
    std::vector<std::pair<Point2, double>> pts;  // bound point X and its parameter t along the line
    const doc::Json points = b.value("points", doc::Json::array());
    for (const auto& p : points) {
        const double t = p.value("t", 0.0);
        Result<dimdetail::RefGeom> g = Error("DRAWING_SECTION_BIND_LOST");
        if (p.contains("vertex")) g = dimdetail::refGeom(c, doc::DimensionRef{"vertex", p["vertex"].get<std::string>()});
        else if (p.contains("center")) g = dimdetail::refGeom(c, doc::DimensionRef{"center", p["center"].get<std::string>()});
        else if (p.contains("mid")) g = dimdetail::refGeom(c, doc::DimensionRef{"edge", p["mid"].get<std::string>()});
        else if (p.contains("edge")) g = dimdetail::refGeom(c, doc::DimensionRef{"edge", p["edge"].get<std::string>()});
        if (!g) return lost;
        if (g->kind == dimdetail::RefGeom::Kind::Point) {
            pts.push_back({g->a, t});
        } else if (g->kind == dimdetail::RefGeom::Kind::Segment) {
            const double at = p.contains("mid") ? 0.5 : p.value("at", 0.5);
            pts.push_back({Point2(g->a + at * (g->b - g->a)), t});
        } else {
            return lost;  // the middle of / a point on a curve edge — not supported
        }
    }
    if (pts.size() == 1) {  // the stored line moved so that its point at t is X
        const Point2 delta = pts[0].first - (s1 + pts[0].second * (s2 - s1));
        return Line(std::array<Point2, 2>{Point2(s1 + delta), Point2(s2 + delta)});
    }
    if (pts.size() != 2) return lost;
    const auto& [X1, t1] = pts[0];
    const auto& [X2, t2] = pts[1];
    if (std::abs(t2 - t1) < 1e-9 || (X2 - X1).norm() < DRAWING_MIN_LINE) return lost;
    auto P = [&](double t) { return Point2(X1 + (t - t1) / (t2 - t1) * (X2 - X1)); };
    return Line(std::array<Point2, 2>{P(0), P(1)});
}

// Resolved view: main view of the chain, frame, effective position and scale, cutting plane, detail view.
struct Resolved {
    const doc::DrawingView* base = nullptr;
    Frame frame;
    Point2 pos{0, 0};
    double scale = 1;
    std::optional<Cut> cut;
    std::optional<Point2> center;  // detail view: anchor point and circle center (view coordinates)
    double radius = 0;             // detail view: radius, model mm
    std::optional<SectionLine> line;  // section: the effective line (bound — from the model, FX9-DRW-002)
    bool bindLost = false;            // section: the binding did not resolve — the stored line (DRAWING_SECTION_BIND_LOST)
};

// Effective section line (§6.11): bound to parent elements when models are given and the binding resolves.
std::optional<SectionLine> effectiveSectionLine(const doc::DrawingView& v, const doc::DrawingView* parentBase, const Frame& pf,
                                                double parentScale, const ModelProvider* models, bool& lost) {
    lost = false;
    if (!v.lineBind || !models || !parentBase) return sectionLine(v);
    auto bl = boundSectionLine(v, *parentBase, pf, parentScale, *models);
    if (!bl) {
        lost = true;
        return sectionLine(v);
    }
    if (!*bl) return sectionLine(v);
    doc::DrawingView w = v;
    w.line = {{{(**bl)[0].x(), (**bl)[0].y()}, {(**bl)[1].x(), (**bl)[1].y()}}};
    return sectionLine(w);
}

// id — an existing view or parent (none — DRAWING_VIEW_PARENT_NOT_FOUND).
Result<Resolved> resolve(const doc::Sheet& sheet, const std::string& id, double sheetScale, std::vector<std::string>& chain,
                         const ModelProvider* models) {
    const int vi = sheet.viewIndex(id);
    if (vi < 0) return Error("DRAWING_VIEW_PARENT_NOT_FOUND").with("id", id);
    const doc::DrawingView& v = sheet.views[size_t(vi)];
    if (const auto at = std::find(chain.begin(), chain.end(), id); at != chain.end())
        return Error("DRAWING_VIEW_CYCLE").with("ids", std::vector<std::string>(at, chain.end()));
    chain.push_back(id);
    const auto own = v.scale ? doc::scaleValue(*v.scale) : std::nullopt;
    if (v.kind == "base") {
        auto f = baseFrame(v.orientation);
        if (v.orientation == "plane" && v.normal && v.up) {  // projection onto a model plane (§6.1, FX7-DRW-007)
            const Vec3 n((*v.normal)[0], (*v.normal)[1], (*v.normal)[2]), u((*v.up)[0], (*v.up)[1], (*v.up)[2]);
            f = Frame{n, u, u.cross(n)};
        }
        if (!f) return Error("API_INVALID_DEFINITION").with("detail", std::string("orientation"));
        Resolved r;
        r.base = &v;
        r.frame = *f;
        r.pos = Point2(v.position[0], v.position[1]);
        r.scale = own.value_or(sheetScale);
        return r;
    }
    auto p = resolve(sheet, v.parent, sheetScale, chain, models);
    if (!p) return p.error();
    Resolved r = *p;
    r.line.reset();
    r.bindLost = false;
    r.scale = own.value_or(p->scale);
    if (v.kind == "projected") {
        const auto f = projectedFrame(p->frame, v.direction);
        if (!f) return Error("API_INVALID_DEFINITION").with("detail", std::string("direction"));
        r.frame = *f;
        const bool vertical = v.direction == "top" || v.direction == "bottom";  // coordinate shared with the parent — x
        r.pos = vertical ? Point2(p->pos.x(), v.position[1]) : Point2(v.position[0], p->pos.y());
        r.cut.reset();
        r.center.reset();
        return r;
    }
    r.pos = Point2(v.position[0], v.position[1]);
    if (v.kind == "section") {  // §6.11
        const auto sl = effectiveSectionLine(v, p->base, p->frame, p->scale, models, r.bindLost);
        if (!sl) return Error("DRAWING_SECTION_LINE_DEGENERATE").with("id", v.id);
        r.line = sl;
        const Frame& pf = p->frame;
        const Vec3 D = sl->d.x() * pf.r + sl->d.y() * pf.u, S = sl->s.x() * pf.r + sl->s.y() * pf.u;
        Frame f;
        f.n = -S;
        if (std::abs(sl->d.x()) >= std::abs(sl->d.y())) {
            f.r = (sl->d.x() >= 0 ? 1.0 : -1.0) * D;
            f.u = f.n.cross(f.r);
        } else {
            f.u = (sl->d.y() >= 0 ? 1.0 : -1.0) * D;
            f.r = f.u.cross(f.n);
        }
        r.frame = f;
        r.cut = Cut{Point3(sl->p1.x() * pf.r + sl->p1.y() * pf.u), f.n, v.sectionOnly};
        r.center.reset();
        return r;
    }
    if (v.kind == "detail") {  // §6.12: frame, plane and section — of the parent
        r.center = Point2(v.center[0], v.center[1]);
        r.radius = v.radius;
        return r;
    }
    return Error("API_INVALID_DEFINITION").with("detail", std::string("kind"));
}

Result<Resolved> resolve(const doc::Sheet& sheet, const std::string& id, double sheetScale, const ModelProvider* models = nullptr) {
    std::vector<std::string> chain;
    return resolve(sheet, id, sheetScale, chain, models);
}

// Projection curve → sheet primitive (§6.6).
Primitive curvePrimitive(const kernel::ViewCurve& c, const detail::ToSheet& toSheet, double k, LineType lt, Weight w, const std::string& role) {
    Primitive p;
    p.lineType = lt;
    p.weight = w;
    p.role = role;
    switch (c.kind) {
        case kernel::ViewCurve::Kind::Line:
            p.kind = PrimitiveKind::Line;
            p.points = {toSheet(c.p1), toSheet(c.p2)};
            break;
        case kernel::ViewCurve::Kind::Circle:
            p.kind = PrimitiveKind::Circle;
            p.center = toSheet(c.center);
            p.radius = k * c.radius;
            break;
        case kernel::ViewCurve::Kind::Arc:
            p.kind = PrimitiveKind::Arc;
            p.center = toSheet(c.center);
            p.radius = k * c.radius;
            p.start = c.start;
            p.end = c.end;
            break;
        case kernel::ViewCurve::Kind::Polyline:
            p.kind = PrimitiveKind::Polyline;
            for (const auto& q : c.points) p.points.push_back(toSheet(q));
            break;
    }
    return p;
}

// Section designation on the parent (§6.13).
void appendSectionLine(const SectionLine& sl, const ViewPlacement& parent, const std::string& letter, const std::string& role,
                       std::vector<Primitive>& out) {
    const Point2 P[2] = {parent.offset + parent.scale * sl.p1, parent.offset + parent.scale * sl.p2};
    const Point2 outward[2] = {-sl.d, sl.d};
    const double h = DESIGNATION_TEXT_MM;
    for (int i = 0; i < 2; ++i) {
        const Point2& o = outward[i];
        const Point2 E = P[i] + SECTION_STROKE_MM * o;
        out.push_back(detail::makeLine(P[i], E, LineType::Solid, Weight::Thick, role));
        const Point2 B = E - SECTION_ARROW_OFFSET_MM * o, T = B + SECTION_ARROW_LEN_MM * sl.s;
        out.push_back(detail::makeLine(B, T, LineType::Solid, Weight::Thin, role));
        Primitive head;
        head.kind = PrimitiveKind::Polyline;
        head.closed = true;
        head.points = {T, T - ARROW_HEAD_LEN_MM * sl.s + ARROW_HEAD_HALF_MM * sl.d, T - ARROW_HEAD_LEN_MM * sl.s - ARROW_HEAD_HALF_MM * sl.d};
        head.role = role;
        out.push_back(std::move(head));
        out.push_back(detail::makeText(B + (SECTION_ARROW_OFFSET_MM + 0.6 * h) * o + 0.5 * SECTION_ARROW_LEN_MM * sl.s, letter, h,
                                       "center middle", role));
    }
}

// Detail view designation on the parent (§6.13).
void appendDetailMark(const doc::DrawingView& v, const ViewPlacement& parent, const std::string& letter, const std::string& role,
                      std::vector<Primitive>& out) {
    const Point2 Cp = parent.offset + parent.scale * Point2(v.center[0], v.center[1]);
    const double R = v.radius * parent.scale, h = DESIGNATION_TEXT_MM;
    Primitive c;
    c.kind = PrimitiveKind::Circle;
    c.center = Cp;
    c.radius = R;
    c.role = role;
    out.push_back(std::move(c));
    const Point2 dir45(std::sqrt(0.5), std::sqrt(0.5));
    const Point2 Q0 = Cp + R * dir45, Q1 = Q0 + DETAIL_LEADER_MM * dir45;
    out.push_back(detail::makeLine(Q0, Q1, LineType::Solid, Weight::Thin, role));
    double wd = 0.7 * h;  // font not loaded — 0.7·h per character (§6.13)
    if (const auto w = textWidth(letter, h)) wd = *w;
    else {
        size_t chars = 0;  // UTF-8 characters
        for (unsigned char ch : letter) chars += (ch & 0xC0) != 0x80;
        wd = 0.7 * h * double(chars);
    }
    out.push_back(detail::makeLine(Q1, Q1 + Point2(wd + 2, 0), LineType::Solid, Weight::Thin, role));
    out.push_back(detail::makeText(Q1 + Point2(1, 1), letter, h, "left bottom", role));
}

}  // namespace

Result<std::vector<kernel::PlacedShape>> ModelProvider::modelBodies(const std::string& file, const std::string& element) const {
    auto st = model(file, element);
    if (!st) return st.error();
    std::vector<kernel::PlacedShape> out;
    if (*st)
        for (const auto& b : (*st)->bodies) out.push_back({b.shape, {}});
    return out;
}

Result<std::shared_ptr<const kernel::HlrProjection>> ModelProvider::projection(const std::vector<kernel::PlacedShape>& bodies,
                                                                              const kernel::ViewDirection& view,
                                                                              const kernel::HlrParams& params) const {
    auto r = kernel().hiddenLineProjection(bodies, view, params);
    if (!r) return r.error();
    return std::make_shared<const kernel::HlrProjection>(std::move(*r));
}

namespace {
// Flat pattern states by model state (§6.21): the same state gives the same flat bodies (projection cache, §6.3).
struct FlatCache {
    std::mutex mu;
    std::vector<std::pair<std::weak_ptr<const regen::PartState>, regen::StatePtr>> items;  // newest last
};
FlatCache& flatCache() {
    static FlatCache c;
    return c;
}
regen::StatePtr cachedFlatState(const kernel::Kernel& k, const regen::StatePtr& st) {
    auto& c = flatCache();
    {
        std::lock_guard lk(c.mu);
        for (const auto& [w, flat] : c.items)
            if (auto s = w.lock(); s == st) return flat;
    }
    regen::StatePtr flat = regen::sheetFlatState(k, *st);
    std::lock_guard lk(c.mu);
    std::erase_if(c.items, [](const auto& it) { return it.first.expired(); });
    if (c.items.size() >= FLAT_VIEW_CACHE) c.items.erase(c.items.begin());
    c.items.push_back({st, flat});
    return flat;
}
}  // namespace

Result<regen::StatePtr> viewModel(const ModelProvider& models, const doc::DrawingView& base) {
    auto st = models.model(base.refFile, regen::drawingModelElement(base.refElement, base.exploded));  // §6.24
    if (!st || !base.flat) return st;
    regen::StatePtr flat = *st ? cachedFlatState(models.kernel(), *st) : nullptr;
    if (!flat) {
        Error e("DRAWING_VIEW_NOT_SHEET_METAL");
        e.with("element", base.refElement);
        if (!base.refFile.empty()) e.with("file", base.refFile);
        return e;
    }
    return flat;
}

Result<std::vector<kernel::PlacedShape>> viewModelBodies(const ModelProvider& models, const doc::DrawingView& base,
                                                         const std::optional<std::vector<std::string>>& sections) {
    // The bodies with the view's model sections (§6.26, S7F-DRW-005, FX10-DRW-003); a provider without the variant — the
    // model's bodies.
    if (!base.flat) {
        const std::string element = regen::drawingModelElement(base.refElement, base.exploded);
        auto cut = models.modelBodies(base.refFile, regen::drawingSectionsElement(element, sections));
        return cut ? cut : models.modelBodies(base.refFile, element);
    }
    auto st = viewModel(models, base);
    if (!st) return st.error();
    std::vector<kernel::PlacedShape> out;
    for (const auto& b : (*st)->bodies) out.push_back({b.shape, {}});
    return out;
}

Result<std::vector<kernel::PlacedShape>> wholeModelBodies(const ModelProvider& models, const doc::DrawingView& base) {
    if (!base.flat) return models.modelBodies(base.refFile, regen::drawingModelElement(base.refElement, base.exploded));
    return viewModelBodies(models, base, std::nullopt);
}

std::optional<Point2> effectiveViewPosition(const doc::Sheet& sheet, const std::string& viewId) {
    if (sheet.viewIndex(viewId) < 0) return std::nullopt;
    auto r = resolve(sheet, viewId, 1.0);
    if (!r) return std::nullopt;
    return r->pos;
}

void appendViews(const doc::Document&, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models, SheetGraphics& out) {
    const double sheetScale = doc::scaleValue(sheet.scale).value_or(1.0);
    const auto letters = viewLetters(drawing);
    auto letterOf = [&](const std::string& id) {
        const auto it = letters.find(id);
        return it == letters.end() ? std::string() : it->second;
    };
    for (const auto& v : sheet.views) {
        auto rv = resolve(sheet, v.id, sheetScale, &models);
        if (!rv) {
            out.errors.push_back({v.id, rv.error()});
            continue;
        }
        if (v.kind == "section" && rv->bindLost) out.warnings.push_back({v.id, Error("DRAWING_SECTION_BIND_LOST").with("id", v.id)});
        const doc::DrawingView& base = *rv->base;
        const auto sectionsList = doc::viewSectionsList(sheet, v.id);  // §6.26, FX10-DRW-003
        auto bodies = viewModelBodies(models, base, sectionsList);
        if (!bodies) {
            if (bodies.error().code == VIEW_PENDING) out.pending.push_back(v.id);
            else out.errors.push_back({v.id, bodies.error()});
            continue;
        }
        // Cut by model sections (§6.26): the bodies differ from the whole model's.
        auto whole = wholeModelBodies(models, base);
        const bool cutByModel = whole && (whole->size() != bodies->size() ||
                                          !std::equal(whole->begin(), whole->end(), bodies->begin(),
                                                      [](const kernel::PlacedShape& a, const kernel::PlacedShape& b) { return a.shape == b.shape; }));
        // Gear rims (§6.20): spaces filled up to the tip circle — teeth are not drawn in views.
        regen::StatePtr modelState;
        if (auto st = viewModel(models, base); st && *st) modelState = *st;
        if (modelState && !modelState->gears.empty()) *bodies = detail::gearConventionalBodies(models.kernel(), *bodies, modelState->gears);
        // Model center (§6.3): union of body boxes transformed by placements — of the whole model when the view is cut by
        // model sections (§6.26), so views with different section lists stay aligned.
        const std::vector<kernel::PlacedShape>& boxBodies = cutByModel ? *whole : *bodies;
        BBox3 box;
        for (const auto& b : boxBodies) {
            const BBox3 bb = models.kernel().bbox(b.shape);
            if (bb.isEmpty()) continue;
            Eigen::Matrix3d R;
            const auto& m = b.motion.r;
            R << m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8];
            for (int c = 0; c < 8; ++c) box.extend(R * bb.corner(static_cast<BBox3::CornerType>(c)) + b.motion.t);
        }
        if (box.isEmpty()) continue;  // model without bodies — view without lines (§6.3)
        const Point3 C = box.center();
        const Frame& f = rv->frame;
        const double k = rv->scale;
        const double chordTol = std::clamp(DRAWING_CURVE_TOL / k, kernel::KERNEL_MIN_CHORD, kernel::KERNEL_MAX_CHORD);
        const kernel::ViewDirection dir{f.n, f.u};
        // Sectioning (§6.11).
        const std::vector<kernel::PlacedShape>* drawBodies = &*bodies;
        std::shared_ptr<const kernel::SectionCut> sc;
        if (rv->cut) {
            auto r = models.sectionCut(*bodies, dir, rv->cut->origin, {chordTol});
            if (!r) {
                if (r.error().code == VIEW_PENDING) out.pending.push_back(v.id);
                else out.errors.push_back({v.id, r.error()});
                continue;
            }
            sc = *r;
            drawBodies = &sc->bodies;
        }
        const bool sectionOnly = rv->cut && rv->cut->sectionOnly;
        std::shared_ptr<const kernel::HlrProjection> pr;
        if (!sectionOnly) {
            if (drawBodies->empty()) {
                pr = std::make_shared<const kernel::HlrProjection>();
            } else {
                auto r = models.projection(*drawBodies, dir, {chordTol});
                if (!r) {
                    if (r.error().code == VIEW_PENDING) out.pending.push_back(v.id);
                    else out.errors.push_back({v.id, r.error()});
                    continue;
                }
                pr = *r;
            }
            for (const auto& w : pr->warnings) out.warnings.push_back({v.id, w});
        }
        // Conversion to the sheet (§6.5): anchor point — model center or detail view center.
        const Point2 anchor = rv->center ? *rv->center : Point2(C.dot(f.r), C.dot(f.u));
        const Point2 pos = rv->pos;
        const detail::ToSheet toSheet = [pos, k, anchor](const Point2& p) { return Point2(pos + k * (p - anchor)); };
        out.placements.push_back({v.id, Point2(pos - k * anchor), k});
        const std::string prefix = "view:" + v.id + ":";
        std::vector<Primitive> prims;
        auto emit = [&](const std::vector<kernel::ViewCurve>& curves, LineType lt, Weight w, const char* cls) {
            for (const auto& c : curves) prims.push_back(curvePrimitive(c, toSheet, k, lt, w, prefix + cls));
        };
        // Drawing order (§6.6).
        if (sc) detail::appendHatch(sc->regions, toSheet, pos, prefix + "hatch", prims);
        // Cut faces of the model sections (§6.26, FX10-DRW-002): planar faces turned to the viewer, projected to the view.
        if (cutByModel && !rv->cut) {
            const std::string element = regen::drawingModelElement(base.refElement, base.exploded);
            std::vector<kernel::SectionRegion> regions;
            for (const auto& cf : models.modelCutFaces(base.refFile, regen::drawingSectionsElement(element, sectionsList))) {
                if (cf.normal.dot(f.n) <= CUT_FACE_MIN_COS) continue;
                kernel::SectionRegion r;
                r.item = cf.body;
                for (const auto& l : cf.loops) {
                    std::vector<Point2> q;
                    for (const auto& P : l) q.emplace_back(P.dot(f.r), P.dot(f.u));
                    r.loops.push_back(std::move(q));
                }
                regions.push_back(std::move(r));
            }
            detail::appendHatch(regions, toSheet, pos, prefix + "hatch", prims);
        }
        // Shaded display (§6.22): the image under the visible lines; hidden and tangent lines — only for pen outputs
        // (the view "as on the drawing" there instead of the image).
        const bool render = v.display == "render" && (v.kind == "base" || v.kind == "projected");
        if (render) {
            std::vector<std::string> colors;
            if (!base.flat) {
                const std::string element = regen::drawingModelElement(base.refElement, base.exploded);
                colors = models.modelColors(base.refFile, regen::drawingSectionsElement(element, sectionsList));
                if (colors.empty()) colors = models.modelColors(base.refFile, element);
            }
            if (colors.size() != drawBodies->size()) colors.clear();  // gear rims replaced the bodies — default colors
            detail::appendShading(models.kernel(), *drawBodies, colors, f, toSheet, k, prefix + "shade", prims);
        }
        const size_t penFrom = prims.size();
        if (pr && v.hiddenLines) emit(pr->hidden, LineType::Dashed, Weight::Thin, "hidden");
        if (pr && v.tangentEdges) emit(pr->visibleSmooth, LineType::Solid, Weight::Thin, "tangent");
        if (render)
            for (size_t i = penFrom; i < prims.size(); ++i) prims[i].penOnly = true;
        if (v.centerLines) detail::appendCenterLines(models.kernel(), *drawBodies, f, toSheet, k, rv->cut, prefix + "center", prims);
        if (modelState && !modelState->threads.empty())
            detail::appendThreads(modelState->threads, f, toSheet, k, rv->cut, v.hiddenLines, pr ? &pr->visible : nullptr, prefix + "thread", prims);
        if (modelState && !modelState->gears.empty()) detail::appendGears(modelState->gears, f, toSheet, k, rv->cut, prefix + "gear", prims);
        if (pr) emit(pr->visible, LineType::Solid, Weight::Thick, "visible");
        if (sectionOnly)
            for (const auto& r : sc->regions)
                for (const auto& l : r.loops) {
                    Primitive p;
                    p.kind = PrimitiveKind::Polyline;
                    p.closed = true;
                    for (const auto& q : l) p.points.push_back(toSheet(q));
                    p.lineType = LineType::Solid;
                    p.weight = Weight::Thick;
                    p.role = prefix + "visible";
                    prims.push_back(std::move(p));
                }
        if (rv->center) {  // detail view: circle clipping and boundary (§6.12)
            const double R = rv->radius * k;
            std::vector<Primitive> clipped;
            for (const auto& p : prims) detail::clipToCircle(p, pos, R, clipped);
            prims = std::move(clipped);
            Primitive b;
            b.kind = PrimitiveKind::Circle;
            b.center = pos;
            b.radius = R;
            b.lineType = LineType::Wavy;
            b.role = prefix + "boundary";
            prims.push_back(std::move(b));
        }
        // Label above the view (§6.13).
        if (v.kind == "section" || v.kind == "detail") {
            const std::string L = letterOf(v.id);
            std::string label;
            if (v.kind == "section") {
                label = L + "\xE2\x80\x93" + L;  // "Б–Б", en dash U+2013
                if (v.scale)
                    if (const auto sv = doc::scaleValue(*v.scale); sv && *sv != sheetScale) label += " (" + detail::scaleText(*v.scale) + ")";
            } else {
                label = L + " (" + detail::scaleText(v.scale.value_or(sheet.scale)) + ")";
            }
            const auto bb = detail::primitivesBox(prims);
            const Point2 at = bb.isEmpty() ? pos : Point2(bb.center().x(), bb.max().y() + LABEL_GAP_MM);
            prims.push_back(detail::makeText(at, label, DESIGNATION_TEXT_MM, "center bottom", prefix + "label"));
        }
        for (auto& p : prims) out.primitives.push_back(std::move(p));
    }
    // Section and detail view designations on the parents (§6.13).
    for (const auto& v : sheet.views) {
        if (v.kind != "section" && v.kind != "detail") continue;
        const auto pl = std::find_if(out.placements.begin(), out.placements.end(), [&](const ViewPlacement& p) { return p.view == v.parent; });
        if (pl == out.placements.end()) continue;
        if (v.kind == "section") {
            const auto rv = resolve(sheet, v.id, sheetScale, &models);  // the effective (bound) line, FX9-DRW-002
            const auto sl = rv && rv->line ? rv->line : sectionLine(v);
            if (sl) appendSectionLine(*sl, *pl, letterOf(v.id), "view:" + v.parent + ":sectionLine", out.primitives);
        } else if (v.radius > 0) {
            appendDetailMark(v, *pl, letterOf(v.id), "view:" + v.parent + ":detailMark", out.primitives);
        }
    }
}

Result<ViewTriad> viewTriad(const doc::Sheet& sheet, const std::string& viewId) {
    if (sheet.viewIndex(viewId) < 0) return Error("DRAWING_VIEW_PARENT_NOT_FOUND").with("id", viewId);
    auto rv = resolve(sheet, viewId, 1.0);
    if (!rv) return rv.error();
    ViewTriad t;
    t.base = rv->base;
    t.n = rv->frame.n;
    t.u = rv->frame.u;
    t.r = rv->frame.r;
    if (rv->cut) {
        t.cutOrigin = rv->cut->origin;
        t.sectionOnly = rv->cut->sectionOnly;
    }
    return t;
}

Result<ViewMapping> viewMapping(const doc::Sheet& sheet, const std::string& viewId, const ModelProvider& models) {
    if (sheet.viewIndex(viewId) < 0) return Error("DRAWING_VIEW_PARENT_NOT_FOUND").with("id", viewId);
    auto rv = resolve(sheet, viewId, doc::scaleValue(sheet.scale).value_or(1.0), &models);
    if (!rv) return rv.error();
    auto bodies = wholeModelBodies(models, *rv->base);  // the anchor — of the whole model, as in appendViews (§6.26)
    if (!bodies) return bodies.error();
    ViewMapping m;
    m.base = rv->base;
    m.n = rv->frame.n;
    m.u = rv->frame.u;
    m.r = rv->frame.r;
    m.scale = rv->scale;
    if (rv->cut) m.cutOrigin = rv->cut->origin;
    // Anchor point (§6.5) — detail view center or model center (§6.3), as in appendViews.
    Point2 anchor(0, 0);
    if (rv->center) {
        anchor = *rv->center;
    } else {
        BBox3 box;
        for (const auto& b : *bodies) {
            const BBox3 bb = models.kernel().bbox(b.shape);
            if (bb.isEmpty()) continue;
            Eigen::Matrix3d R;
            const auto& mr = b.motion.r;
            R << mr[0], mr[1], mr[2], mr[3], mr[4], mr[5], mr[6], mr[7], mr[8];
            for (int c = 0; c < 8; ++c) box.extend(R * bb.corner(static_cast<BBox3::CornerType>(c)) + b.motion.t);
        }
        if (!box.isEmpty()) anchor = m.toView(box.center());
    }
    m.offset = rv->pos - m.scale * anchor;
    return m;
}

}  // namespace construe::drawing
