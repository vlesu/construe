// Cut plan (spec/drawing.md §2.7, §2.7.1; FX11-CUT-001…006).
#include "drawing/cut_plan.h"

#include "base/number_format.h"
#include "base/spline.h"
#include "drawing/render.h"
#include "drawing/standard.h"
#include "sheet/sheet.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <set>

namespace construe::drawing {

namespace {

constexpr double TWO_PI = 2 * std::numbers::pi;
constexpr double JOIN_TOL = 1e-4;   // as FLAT_JOIN_TOL (io.md §2д.4)
constexpr double SPLINE_TOL = 1e-3;  // as FLAT_SPLINE_TOL
constexpr int ELLIPSE_STEPS = 72;

struct LabelFormat {
    std::mutex mu;
    std::string with = "{material}, {thickness} мм", without = "{thickness} мм", sep = ",";
};
LabelFormat& labelFormat() {
    static LabelFormat f;
    return f;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    return s;
}

double sweepOf(double a0, double a1) {
    double t = std::fmod(a1 - a0, TWO_PI);
    if (t <= 0) t += TWO_PI;
    return t;
}

// Bounding box of a primitive (§2.7 item 4): ends, arc extremes within its angle, circle c ± r.
void extendBox(const Primitive& p, Point2& lo, Point2& hi) {
    auto ext = [&](const Point2& q) { lo = lo.cwiseMin(q), hi = hi.cwiseMax(q); };
    switch (p.kind) {
        case PrimitiveKind::Line:
        case PrimitiveKind::Polyline:
            for (const auto& q : p.points) ext(q);
            break;
        case PrimitiveKind::Circle:
            ext(p.center - Point2(p.radius, p.radius));
            ext(p.center + Point2(p.radius, p.radius));
            break;
        case PrimitiveKind::Arc: {
            const double th = sweepOf(p.start, p.end);
            ext(p.center + p.radius * Point2(std::cos(p.start), std::sin(p.start)));
            ext(p.center + p.radius * Point2(std::cos(p.start + th), std::sin(p.start + th)));
            for (int k = 0; k < 8; ++k) {
                const double a = k * std::numbers::pi / 2;
                double d = std::fmod(a - p.start, TWO_PI);
                if (d < 0) d += TWO_PI;
                if (d <= th) ext(p.center + p.radius * Point2(std::cos(a), std::sin(a)));
            }
            break;
        }
        case PrimitiveKind::Text: ext(p.position); break;
        default: break;
    }
}

Primitive linePrim(const Point2& a, const Point2& b) {
    Primitive p;
    p.kind = PrimitiveKind::Line;
    p.points = {a, b};
    return p;
}
Primitive arcPrim(const Point2& c, double r, const Point2& a, const Point2& b) {
    Primitive p;
    p.kind = PrimitiveKind::Arc;
    p.center = c;
    p.radius = r;
    p.start = std::atan2(a.y() - c.y(), a.x() - c.x());
    p.end = std::atan2(b.y() - c.y(), b.x() - c.x());
    return p;
}
Primitive circlePrim(const Point2& c, double r) {
    Primitive p;
    p.kind = PrimitiveKind::Circle;
    p.center = c;
    p.radius = r;
    return p;
}
Primitive polyPrim(std::vector<Point2> pts, bool closed) {
    Primitive p;
    p.kind = PrimitiveKind::Polyline;
    p.points = std::move(pts);
    p.closed = closed;
    return p;
}

std::vector<Point2> flattenSpline(const spline::Curve& c) {
    std::vector<Point2> pts;
    if (!spline::validate(c).empty()) return pts;
    const spline::Flat f = spline::expand(c);
    for (double t : spline::flattenParams(f, SPLINE_TOL)) {
        const Point2 q = spline::eval(f, t);
        if (!pts.empty() && (q - pts.back()).norm() <= JOIN_TOL) continue;
        pts.push_back(q);
    }
    return pts;
}

// Duplicate of an already taken primitive (§2.7 item 4, io.md §2д.1 item 1): same ends / center, radius and ends.
bool sameAs(const Primitive& a, const Primitive& b) {
    if (a.kind != b.kind) return false;
    auto near = [](const Point2& x, const Point2& y) { return (x - y).norm() <= JOIN_TOL; };
    switch (a.kind) {
        case PrimitiveKind::Line:
            return (near(a.points[0], b.points[0]) && near(a.points[1], b.points[1])) ||
                   (near(a.points[0], b.points[1]) && near(a.points[1], b.points[0]));
        case PrimitiveKind::Circle: return near(a.center, b.center) && std::abs(a.radius - b.radius) <= JOIN_TOL;
        case PrimitiveKind::Arc: {
            auto pt = [](const Primitive& p, double t) { return Point2(p.center + p.radius * Point2(std::cos(t), std::sin(t))); };
            return near(a.center, b.center) && std::abs(a.radius - b.radius) <= JOIN_TOL && near(pt(a, a.start), pt(b, b.start)) &&
                   near(pt(a, a.end), pt(b, b.end));
        }
        case PrimitiveKind::Polyline:
            return a.points.size() == b.points.size() && near(a.points.front(), b.points.front()) && near(a.points.back(), b.points.back()) &&
                   near(a.points[a.points.size() / 2], b.points[b.points.size() / 2]);
        default: return false;
    }
}
void addUnique(std::vector<Primitive>& out, Primitive p) {
    if (std::none_of(out.begin(), out.end(), [&](const Primitive& q) { return sameAs(p, q); })) out.push_back(std::move(p));
}

// Contour of the flat pattern (§2.7 item 4): section curves cleaned as io.md §2д.1 item 1.
std::vector<Primitive> contourOf(const std::vector<kernel::Curve2>& curves) {
    std::vector<Primitive> out;
    for (const auto& c : curves) {
        switch (c.kind) {
            case kernel::Curve2::Kind::Line:
                if ((c.p2 - c.p1).norm() > JOIN_TOL) addUnique(out, linePrim(c.p1, c.p2));
                break;
            case kernel::Curve2::Kind::Circle: addUnique(out, circlePrim(c.center, c.radius)); break;
            case kernel::Curve2::Kind::Arc: {
                const double a0 = std::atan2(c.start.y() - c.center.y(), c.start.x() - c.center.x());
                const double a1 = std::atan2(c.end.y() - c.center.y(), c.end.x() - c.center.x());
                const double th = sweepOf(a0, a1);
                if (th >= TWO_PI - 1e-12 || (c.end - c.start).norm() <= JOIN_TOL) {
                    if (th >= std::numbers::pi || th >= TWO_PI - 1e-12) addUnique(out, circlePrim(c.center, c.radius));
                    break;
                }
                addUnique(out, arcPrim(c.center, c.radius, c.start, c.end));
                break;
            }
            case kernel::Curve2::Kind::Spline: {
                auto pts = flattenSpline(c.spline);
                if (pts.size() >= 2) addUnique(out, polyPrim(std::move(pts), false));
                break;
            }
            default: break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- engraving (§2.7.1)

// Free sketches of the part: not suppressed, no other feature depends on them.
std::vector<std::string> freeSketches(const doc::Element& el) {
    std::set<std::string, IdLess> used;
    for (const auto& f : el.features)
        for (const auto& id : doc::featureDependencies(*f))
            if (id != f->id) used.insert(id);
    std::vector<std::string> out;
    for (const auto& f : el.features)
        if (f->sketchData() && !f->suppressed && !used.count(f->id)) out.push_back(f->id);
    return out;
}

// Points of a wall region outline (approximating arcs by their ends and middle) for the point test.
std::vector<Point2> outlinePoints(const sheet::Loop& loop) {
    std::vector<Point2> pts;
    for (const auto& s : loop) {
        pts.push_back(s.rev ? s.b : s.a);
        if (s.kind == sheet::Seg::Kind::Arc) {
            const double a0 = std::atan2(s.a.y() - s.c.y(), s.a.x() - s.c.x());
            const double th = sweepOf(a0, std::atan2(s.b.y() - s.c.y(), s.b.x() - s.c.x()));
            for (int k = 1; k < 8; ++k) {
                const double t = a0 + th * (s.rev ? 1 - k / 8.0 : k / 8.0);
                pts.push_back(s.c + s.r * Point2(std::cos(t), std::sin(t)));
            }
        }
    }
    return pts;
}
double distToPolygon(const std::vector<Point2>& poly, const Point2& p) {
    double best = 1e300;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2 a = poly[i], b = poly[(i + 1) % poly.size()];
        const Point2 ab = b - a;
        const double L = ab.squaredNorm();
        const double t = L > 0 ? std::clamp((p - a).dot(ab) / L, 0.0, 1.0) : 0.0;
        best = std::min(best, (a + t * ab - p).norm());
    }
    return best;
}
bool insidePolygon(const std::vector<Point2>& poly, const Point2& p) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Point2 &a = poly[i], &b = poly[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()) in = !in;
    }
    return in || (!poly.empty() && distToPolygon(poly, p) <= CUT_PLANE_TOL);
}

// A sketch curve in sketch-local coordinates (lines, arcs counterclockwise about the frame normal, circles, polylines).
struct LocalCurve {
    PrimitiveKind kind = PrimitiveKind::Line;
    std::vector<Point2> pts;  // line: a, b; arc: start, end; polyline: vertices
    Point2 center{0, 0};
    double radius = 0;
    bool closed = false;
};

std::vector<LocalCurve> sketchCurves(const regen::SketchEntry& se) {
    std::vector<LocalCurve> out;
    std::set<std::string> taken;
    for (const auto& w : se.curves) {
        taken.insert(w.id);
        LocalCurve c;
        switch (w.kind) {
            case regen::WorldCurve::Kind::Line: c.kind = PrimitiveKind::Line, c.pts = {se.frame.toLocal(w.a), se.frame.toLocal(w.b)}; break;
            case regen::WorldCurve::Kind::Arc:
                c.kind = PrimitiveKind::Arc, c.center = se.frame.toLocal(w.center), c.radius = w.radius;
                c.pts = {se.frame.toLocal(w.a), se.frame.toLocal(w.b)};
                if (w.axis.dot(se.frame.normal) < 0) std::swap(c.pts[0], c.pts[1]);  // counterclockwise about the frame normal
                break;
            case regen::WorldCurve::Kind::Circle: c.kind = PrimitiveKind::Circle, c.center = se.frame.toLocal(w.center), c.radius = w.radius; break;
        }
        out.push_back(std::move(c));
    }
    // Outlines of texts, ellipses and splines — from the profile regions (entities not among the curves).
    auto addSeg = [&](const sketch::Segment& s) {
        if (taken.count(s.id)) return;
        LocalCurve c;
        switch (s.kind) {
            case sketch::Segment::Kind::Line: c.kind = PrimitiveKind::Line, c.pts = {s.p1, s.p2}; break;
            case sketch::Segment::Kind::Arc: c.kind = PrimitiveKind::Arc, c.center = s.center, c.radius = s.radius, c.pts = {s.start, s.end}; break;
            case sketch::Segment::Kind::Circle: c.kind = PrimitiveKind::Circle, c.center = s.center, c.radius = s.radius; break;
            case sketch::Segment::Kind::Ellipse:
            case sketch::Segment::Kind::EllipseArc: {
                const bool full = s.kind == sketch::Segment::Kind::Ellipse;
                const Vec2 ux(std::cos(s.angle), std::sin(s.angle)), uy(-ux.y(), ux.x());
                auto at = [&](double t) { return Point2(s.center + s.radius * std::cos(t) * ux + s.minorRadius * std::sin(t) * uy); };
                auto param = [&](const Point2& q) {
                    const Point2 d = q - s.center;
                    return std::atan2(d.dot(uy) / std::max(s.minorRadius, 1e-12), d.dot(ux) / std::max(s.radius, 1e-12));
                };
                const double t0 = full ? 0 : param(s.start), th = full ? TWO_PI : sweepOf(t0, param(s.end));
                c.kind = PrimitiveKind::Polyline;
                c.closed = full;
                const int n = std::max(2, int(std::ceil(ELLIPSE_STEPS * th / TWO_PI)));
                for (int i = 0; i <= (full ? n - 1 : n); ++i) c.pts.push_back(at(t0 + th * i / n));
                break;
            }
            case sketch::Segment::Kind::Spline: {
                c.kind = PrimitiveKind::Polyline;
                c.pts = flattenSpline(s.curve);
                c.closed = s.curve.periodic;
                if (c.pts.size() < 2) return;
                break;
            }
        }
        out.push_back(std::move(c));
    };
    for (const auto& r : se.result.profiles.regions) {
        for (const auto& s : r.outer.segments) addSeg(s);
        for (const auto& h : r.holes)
            for (const auto& s : h.segments) addSeg(s);
    }
    return out;
}

// Engraving of the free sketches of a part (§2.7.1), flat coordinates.
std::vector<Primitive> engravingOf(const regen::PartState& st, const doc::Element& el) {
    std::vector<Primitive> out;
    const auto& m = *st.sheet->model;
    const double T = m.params.thickness;
    for (const auto& id : freeSketches(el)) {
        const auto it = st.sketches.find(id);
        if (it == st.sketches.end()) continue;
        const auto curves = sketchCurves(it->second);
        if (curves.empty()) continue;
        const Plane& fr = it->second.frame;
        std::vector<Point3> probe;  // world points of the sketch curves
        for (const auto& c : curves) {
            for (const auto& q : c.pts) probe.push_back(fr.toWorld(q));
            if (c.kind == PrimitiveKind::Circle) probe.push_back(fr.toWorld(c.center + Point2(c.radius, 0)));
        }
        for (size_t i = 0; i < m.pieces.size(); ++i) {
            const auto& piece = m.pieces[i];
            if (piece.kind != sheet::Piece::Kind::Wall) continue;
            const Transform3 inv = sheet::fold(m, int(i)).inverse();
            const Vec3 n = inv.linear() * fr.normal;
            if (std::abs(n.z()) < 1 - CUT_PLANE_TOL) continue;
            bool onFace = !probe.empty();
            for (const auto& w : probe) {
                const double z = (inv * w).z();
                if (std::abs(z) > CUT_PLANE_TOL && std::abs(z - T) > CUT_PLANE_TOL) onFace = false;
            }
            const double z0 = (inv * probe.front()).z();
            for (const auto& w : probe)
                if (std::abs((inv * w).z() - z0) > CUT_PLANE_TOL) onFace = false;
            if (!onFace) continue;
            const Point3 f0 = inv * probe.front();
            if (!insidePolygon(outlinePoints(piece.region.outer), Point2(f0.x(), f0.y()))) continue;
            auto toFlat = [&](const Point2& q) {
                const Point3 f = inv * fr.toWorld(q);
                return Point2(f.x(), f.y());
            };
            const bool keep = n.z() > 0;  // arc direction kept when the sketch normal looks along +z of the wall
            for (const auto& c : curves) {
                switch (c.kind) {
                    case PrimitiveKind::Line: addUnique(out, linePrim(toFlat(c.pts[0]), toFlat(c.pts[1]))); break;
                    case PrimitiveKind::Circle: addUnique(out, circlePrim(toFlat(c.center), c.radius)); break;
                    case PrimitiveKind::Arc:
                        addUnique(out, keep ? arcPrim(toFlat(c.center), c.radius, toFlat(c.pts[0]), toFlat(c.pts[1]))
                                            : arcPrim(toFlat(c.center), c.radius, toFlat(c.pts[1]), toFlat(c.pts[0])));
                        break;
                    case PrimitiveKind::Polyline: {
                        std::vector<Point2> pts;
                        for (const auto& q : c.pts) pts.push_back(toFlat(q));
                        addUnique(out, polyPrim(std::move(pts), c.closed));
                        break;
                    }
                    default: break;
                }
            }
            break;
        }
    }
    return out;
}

// Cached flat pattern contours per state object (the section is a kernel call).
struct CutCache {
    std::mutex mu;
    std::vector<std::pair<std::weak_ptr<const regen::PartState>, std::shared_ptr<const std::vector<Primitive>>>> items;
};
CutCache& cutCache() {
    static CutCache c;
    return c;
}

const doc::Element* partElement(const doc::Document& d, const ModelProvider& models, const doc::DrawingView& v) {
    const doc::Document* D = v.refFile.empty() ? &d : models.linkedDocument(v.refFile);
    if (!D) return nullptr;
    const int ei = D->elementIndex(v.refElement);
    return ei < 0 ? nullptr : &D->elements[size_t(ei)];
}

std::string materialName(const doc::Element* el) {
    if (!el || !el->material || !el->material->is_object()) return {};
    const auto it = el->material->find("name");
    return it != el->material->end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool isCutPlan(const doc::Element& drawing) { return drawing.standard && *drawing.standard == "cut"; }

void setCutLabelFormat(std::string withMaterial, std::string withoutMaterial, std::string decimalSeparator) {
    auto& f = labelFormat();
    std::lock_guard lk(f.mu);
    f.with = std::move(withMaterial);
    f.without = std::move(withoutMaterial);
    f.sep = std::move(decimalSeparator);
}

std::string cutLabel(const std::string& material, double thickness) {
    auto& f = labelFormat();
    std::lock_guard lk(f.mu);
    const std::string t = replaceAll(fixedTrimmed(thickness, 3), ".", f.sep);
    std::string s = material.empty() ? f.without : f.with;
    s = replaceAll(s, "{thickness}", t);
    return replaceAll(s, "{material}", material);
}

Result<CutPart> cutPart(const kernel::Kernel& k, const regen::StatePtr& st, const doc::Element* el, const std::string& element,
                        const std::string& file) {
    if (!st || !st->sheet || !st->sheet->model || st->sheet->body.empty() || st->sheet->flat.null()) {
        Error e("DRAWING_CUT_NOT_SHEET_METAL");
        e.with("element", element);
        if (!file.empty()) e.with("file", file);
        return e;
    }
    std::shared_ptr<const std::vector<Primitive>> contour;
    auto& c = cutCache();
    {
        std::lock_guard lk(c.mu);
        for (const auto& [w, prims] : c.items)
            if (w.lock() == st) contour = prims;
    }
    if (!contour) {
        auto fp = sheet::flatPattern(k, *st->sheet->model, st->sheet->flat);
        if (!fp) return fp.error();
        contour = std::make_shared<const std::vector<Primitive>>(contourOf(fp->curves));
        std::lock_guard lk(c.mu);
        std::erase_if(c.items, [](const auto& it) { return it.first.expired(); });
        if (c.items.size() >= FLAT_VIEW_CACHE) c.items.erase(c.items.begin());
        c.items.push_back({st, contour});
    }
    CutPart p;
    p.thickness = st->sheet->model->params.thickness;
    p.contour = *contour;
    if (el) p.engraving = engravingOf(*st, *el);
    Point2 lo(1e300, 1e300), hi(-1e300, -1e300);
    for (const auto& q : p.contour) extendBox(q, lo, hi);
    for (const auto& q : p.engraving) extendBox(q, lo, hi);
    if (lo.x() > hi.x()) lo = hi = Point2(0, 0);
    p.min = lo;
    p.max = hi;
    return p;
}

std::optional<double> cutSheetThickness(const doc::Document& d, const doc::Sheet& sheet, const ModelProvider& models) {
    for (const auto& v : sheet.views) {
        if (v.kind != "base") continue;
        auto st = models.model(v.refFile, v.refElement);
        if (!st) continue;
        auto p = cutPart(models.kernel(), *st, nullptr, v.refElement, v.refFile);
        (void)d;
        if (p) return p->thickness;
    }
    return std::nullopt;
}

Point2 cutRowPosition(const doc::Sheet& sheet, const SheetGraphics& g, double w, double h) {
    double xmax = CUT_SHEET_MARGIN_MM - CUT_LAYOUT_GAP_MM;
    for (const auto& p : g.primitives) {
        bool part = false;
        for (const auto& v : sheet.views)
            if (p.role.rfind("view:" + v.id + ":", 0) == 0) part = true;
        if (!part) continue;
        Point2 lo(1e300, 1e300), hi(-1e300, -1e300);
        extendBox(p, lo, hi);
        if (p.kind == PrimitiveKind::Text)
            if (auto tw = textWidth(p.text, p.height, p.font); tw) hi.x() = std::max(hi.x(), p.position.x() + *tw / 2);
        if (lo.x() <= hi.x()) xmax = std::max(xmax, hi.x());
    }
    return Point2(xmax + CUT_LAYOUT_GAP_MM + w / 2, CUT_SHEET_MARGIN_MM + h / 2);
}

SheetGraphics composeCutSheet(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models) {
    (void)drawing;
    SheetGraphics g;
    const std::string font = drawingFontFor(font::DrawingStandard::Gost);  // §2.7 item 9
    const DrawingFontScope fontScope(font);
    std::optional<double> sheetT;
    for (const auto& v : sheet.views) {
        if (v.kind != "base") {
            g.errors.push_back({v.id, Error("DRAWING_CUT_VIEW_KIND").with("id", v.id)});
            continue;
        }
        auto st = models.model(v.refFile, v.refElement);
        if (!st) {
            if (st.error().code == VIEW_PENDING) g.pending.push_back(v.id);
            else g.errors.push_back({v.id, st.error()});
            continue;
        }
        const doc::Element* el = partElement(d, models, v);
        auto part = cutPart(models.kernel(), *st, el, v.refElement, v.refFile);
        if (!part) {
            g.errors.push_back({v.id, part.error()});
            continue;
        }
        if (!sheetT) sheetT = part->thickness;
        else if (std::abs(part->thickness - *sheetT) > CUT_THICKNESS_TOL) {
            Error e("DRAWING_CUT_THICKNESS");
            e.with("element", v.refElement).with("thickness", part->thickness).with("sheet", *sheetT);
            g.errors.push_back({v.id, e});
            continue;
        }
        const Point2 anchor = (part->min + part->max) / 2;
        const Point2 pos(v.position[0], v.position[1]);
        const Point2 off = pos - anchor;
        auto place = [&](Primitive p, const std::string& cls, const char* color) {
            for (auto& q : p.points) q += off;
            p.center += off;
            p.lineType = LineType::Solid;
            p.weight = Weight::Thin;
            p.role = "view:" + v.id + ":" + cls;
            p.color = color;
            g.primitives.push_back(std::move(p));
        };
        for (const auto& p : part->contour) place(p, "cut", CUT_COLOR_CONTOUR);
        for (const auto& p : part->engraving) place(p, "engrave", CUT_COLOR_ENGRAVE);
        Primitive label;
        label.kind = PrimitiveKind::Text;
        label.text = cutLabel(materialName(el), part->thickness);
        label.position = Point2(pos.x(), part->min.y() + off.y() - CUT_LABEL_GAP_MM);
        label.height = CUT_LABEL_MM;
        label.align = "center top";
        label.role = "view:" + v.id + ":cutLabel";
        label.color = CUT_COLOR_LABEL;
        g.primitives.push_back(std::move(label));
        g.placements.push_back({v.id, off, 1.0});
    }
    // Texts of the sheet (§6.23) — the label color (FX11-CUT-005).
    const size_t textsFrom = g.primitives.size();
    appendTexts(sheet, g);
    for (size_t i = textsFrom; i < g.primitives.size(); ++i) g.primitives[i].color = CUT_COLOR_LABEL;
    applyDrawingFont(g.primitives, font);
    // Sheet size (§2.7 item 8): grows right and up so that a margin stays.
    double X = 0, Y = 0;
    for (const auto& p : g.primitives) {
        Point2 lo(1e300, 1e300), hi(-1e300, -1e300);
        extendBox(p, lo, hi);
        if (p.kind == PrimitiveKind::Text) {
            if (auto tw = textWidth(p.text, p.height, p.font, p.styles); tw) {
                const std::string h = p.align.substr(0, p.align.find(' '));
                const double w = *tw * std::max(std::abs(std::cos(p.angle)), std::abs(std::sin(p.angle)));
                hi.x() = p.position.x() + (h == "center" ? w / 2 : h == "right" ? 0 : w);
            }
            if (p.align.find("top") == std::string::npos) hi.y() = p.position.y() + p.height;
        }
        if (lo.x() > hi.x()) continue;
        X = std::max(X, hi.x());
        Y = std::max(Y, hi.y());
    }
    g.width = std::max(CUT_SHEET_W, X + CUT_SHEET_MARGIN_MM);
    g.height = std::max(CUT_SHEET_H, Y + CUT_SHEET_MARGIN_MM);
    return g;
}

}  // namespace construe::drawing
