// Technical requirement annotations of models on drawing views (spec/drawing.md §6.18) and the tolerance from the model
// in a dimension label (§6а.3): surface roughness (GOST 2.309), form and location tolerance frame (GOST 2.308), datum.
#include "drawing/view_pmi.h"

#include "drawing/dimensions.h"
#include "drawing/render.h"
#include "drawing/sheet_marks.h"
#include "drawing/weld_annotations.h"
#include "drawing/standard.h"
#include "pmi/pmi.h"
#include "regen/naming.h"
#include "base/constants.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <memory>
#include <utility>

namespace construe::drawing {

namespace {

// Constants of §6.18.
constexpr double PMI_TEXT_MM = 3.5;
constexpr double PMI_EDGE_ON_DEG = 1;
constexpr double PMI_LEADER_MM = 7;
constexpr double PMI_STACK_MM = 12;
constexpr double PMI_DOT_MM = 0.5;
constexpr double PMI_EPS = 1e-6;
// Constants of §6.18а (roughness of the remaining surfaces); sizes except the margin are in units of h.
constexpr double REST_TEXT_MM = 1.5 * PMI_TEXT_MM;
constexpr double REST_MARGIN_MM = 5;
constexpr double REST_GAP = 1;
constexpr double REST_PAD = 0.5;
constexpr double REST_BRACKET_R = 3.5;
constexpr double REST_BRACKET_HALF = 1.75;

bool partLike(const doc::Element& el) { return !el.isAssembly() && !el.isDrawing() && !el.isSpecification(); }
// Models whose annotations go to drawings: parts, bodies and assemblies (instance faces, FX12-PMI-003).
bool annotatable(const doc::Element& el) { return !el.isDrawing() && !el.isSpecification(); }

// Text width (§3.1); font not loaded — 0.7·h per character (§6.13).
double widthOf(const std::string& t, double h) {
    if (t.empty()) return 0;
    if (const auto w = textWidth(t, h)) return *w;
    size_t chars = 0;
    for (unsigned char ch : t) chars += (ch & 0xC0) != 0x80;
    return 0.7 * h * double(chars);
}

// Candidate view (§6.18): sheet, view, frame.
struct Candidate {
    const doc::Sheet* sheet = nullptr;
    const doc::DrawingView* view = nullptr;
    ViewTriad triad;
};

enum class ViewClass { None, Contour, Front };
enum class ContourKind { EdgeOn, CylinderSide, CylinderEnd };

struct Classified {
    ViewClass cls = ViewClass::None;
    ContourKind kind = ContourKind::EdgeOn;
};

Classified classify(const detail::AnnotationFace& af, const ViewTriad& t) {
    const double s = std::sin(PMI_EDGE_ON_DEG * PI / 180), c = std::cos(PMI_EDGE_ON_DEG * PI / 180);
    if (af.surface.kind == kernel::SurfaceInfo::Kind::Cylinder) {
        const double an = std::abs(af.surface.dir.normalized().dot(t.n));
        if (an <= s) return {ViewClass::Contour, ContourKind::CylinderSide};
        if (an >= c) return {ViewClass::Contour, ContourKind::CylinderEnd};
    } else if (std::abs(af.normal.dot(t.n)) <= s) {
        return {ViewClass::Contour, ContourKind::EdgeOn};
    }
    if (af.normal.dot(t.n) >= s) return {ViewClass::Front, ContourKind::EdgeOn};
    return {};
}

// Annotation primitives (§6.18): all solid, thin, one role.
struct Out {
    std::string role;
    std::vector<Primitive>& prims;
    void line(const Point2& a, const Point2& b) {
        Primitive p;
        p.kind = PrimitiveKind::Line;
        p.points = {a, b};
        p.role = role;
        prims.push_back(std::move(p));
    }
    void poly(std::vector<Point2> pts, bool closed, bool filled) {
        Primitive p;
        p.kind = PrimitiveKind::Polyline;
        p.points = std::move(pts);
        p.closed = closed;
        p.filled = filled;
        p.role = role;
        prims.push_back(std::move(p));
    }
    void arc(const Point2& c, double r, double start, double end) {
        Primitive p;
        p.kind = PrimitiveKind::Arc;
        p.center = c;
        p.radius = r;
        p.start = start;
        p.end = end;
        p.role = role;
        prims.push_back(std::move(p));
    }
    void circle(const Point2& c, double r) {
        Primitive p;
        p.kind = PrimitiveKind::Circle;
        p.center = c;
        p.radius = r;
        p.role = role;
        prims.push_back(std::move(p));
    }
    void text(const std::string& t, const Point2& at, double height, const char* align) {
        if (t.empty()) return;
        Primitive p;
        p.kind = PrimitiveKind::Text;
        p.position = at;
        p.height = height;
        p.text = t;
        p.align = align;
        p.role = role;
        prims.push_back(std::move(p));
    }
    // Arrow of §6а.4: tip at tip, tail in direction b.
    void arrow(const Point2& tip, const Point2& b) {
        const double hh = DIM_ARROW_LEN * std::tan(DIM_ARROW_ANGLE_DEG * PI / 180 / 2);
        const Point2 n(-b.y(), b.x());
        poly({tip, tip + DIM_ARROW_LEN * b + hh * n, tip + DIM_ARROW_LEN * b - hh * n}, true, true);
    }
    void dot(const Point2& c) {
        std::vector<Point2> pts;
        for (int i = 0; i < 12; ++i) pts.push_back(c + PMI_DOT_MM * Point2(std::cos(i * PI / 6), std::sin(i * PI / 6)));
        poly(std::move(pts), true, true);
    }
    // pmi §6 symbol: point (x, y) → origin + scale·(x, y).
    void glyph(const pmi::Glyph& g, const Point2& origin, double scale) {
        for (const auto& pl : g.polylines) {
            std::vector<Point2> pts;
            for (const auto& q : pl) pts.push_back(origin + scale * q);
            poly(std::move(pts), false, false);
        }
        for (const auto& c : g.circles) circle(origin + scale * c.center, scale * c.r);
        for (const auto& f : g.filled) {
            std::vector<Point2> pts;
            for (const auto& q : f) pts.push_back(origin + scale * q);
            poly(std::move(pts), true, true);
        }
    }
};

// Leader start and leader (§6.18): arrow at the outline or dot inside the outline, the line S–E.
void leader(Out& out, const Point2& S, const Point2& E, const Point2& o, bool contour) {
    if (contour) out.arrow(S, o);
    else out.dot(S);
    out.line(S, E);
}

// Sign placement on a view (§6.18, §6.25): start S, direction o, leader end E; contour — arrow start; free — the
// position is given (roughness always on the leader shelf, the datum line up to E).
struct Place {
    Point2 S{0, 0}, o{0, 1}, E{0, 0};
    bool contour = false, free = false;
};

// Returns the anchor (§6.25): the leader end E, or the sign vertex when the sign stands on the outline.
Point2 roughness(Out& out, const doc::Json& a, const Place& p) {
    const double h = PMI_TEXT_MM;
    const std::string param = pmi::roughnessText(a);
    const std::string method = a.value("method", std::string());
    const std::string lay = pmi::laySymbol(a.value("lay", std::string()));
    const bool shelf = a.contains("parameter") || a.contains("method") || a.contains("lay");
    const double tw = std::max(widthOf(param, h), widthOf(method, h)) / h;
    const pmi::Glyph g = pmi::roughnessGlyph(a.value("sign", std::string("any")), tw, shelf);
    Point2 V = p.S, anchor = p.S;
    if (p.free || !(p.contour && p.o.y() >= std::cos(PMI_EDGE_ON_DEG * PI / 180))) {  // on the leader shelf
        leader(out, p.S, p.E, p.o, p.contour);
        const Point2& E = p.E;
        anchor = E;
        const double xl = -0.577, xr = 1.732 + (shelf ? tw + 0.4 : 0);
        if (p.o.x() >= 0) {
            V = E + h * Point2(0.5 - xl, 0);
            out.line(E, Point2(V.x() + h * xr, E.y()));
        } else {
            V = E - h * Point2(0.5 + xr, 0);
            out.line(Point2(V.x() + h * xl, E.y()), E);
        }
    }
    out.glyph(g, V, h);
    out.text(param, V + h * Point2(1.9, 1.8), h, "left bottom");
    out.text(method, V + h * Point2(1.9, 3.3), h, "left bottom");
    out.text(lay, V + h * Point2(0.9, 0.1), h, "left bottom");
    return anchor;
}

Point2 geometric(Out& out, const doc::Json& a, const Place& p) {
    const double h = PMI_TEXT_MM;
    leader(out, p.S, p.E, p.o, p.contour);
    const Point2& E = p.E;
    const Point2& o = p.o;
    const std::string val = pmi::frameValueText(a);
    const std::string mat = a.value("material", std::string());
    std::vector<std::string> datums;
    if (a.contains("datums"))
        for (const auto& l : a["datums"]) datums.push_back(l.get<std::string>());
    const double wv = widthOf(val, h), cv = wv + h + (mat.empty() ? 0 : 1.4 * h);
    const double W = 2 * h + cv + 2 * h * double(datums.size());
    double x0 = 0, y0 = 0;
    if (std::abs(o.y()) >= std::abs(o.x())) {
        x0 = E.x() - h;
        y0 = o.y() > 0 ? E.y() : E.y() - 2 * h;
    } else {
        x0 = o.x() > 0 ? E.x() : E.x() - W;
        y0 = E.y() - h;
    }
    out.poly({Point2(x0, y0), Point2(x0 + W, y0), Point2(x0 + W, y0 + 2 * h), Point2(x0, y0 + 2 * h)}, true, false);
    auto sep = [&](double x) { out.line(Point2(x, y0), Point2(x, y0 + 2 * h)); };
    sep(x0 + 2 * h);
    for (size_t i = 0; i < datums.size(); ++i) sep(x0 + 2 * h + cv + 2 * h * double(i));
    out.glyph(pmi::characteristicGlyph(a.value("characteristic", std::string())), Point2(x0, y0), 2 * h);
    out.text(val, Point2(x0 + 2 * h + h / 2, y0 + h), h, "left middle");
    if (!mat.empty()) {
        const Point2 mc(x0 + 2 * h + h / 2 + wv + 0.8 * h, y0 + h);
        out.circle(mc, 0.6 * h);
        out.text(mat, mc, 0.8 * h, "center middle");
    }
    for (size_t i = 0; i < datums.size(); ++i) out.text(datums[i], Point2(x0 + 2 * h + cv + 2 * h * double(i) + h, y0 + h), h, "center middle");
    return E;
}

Point2 datum(Out& out, const doc::Json& a, const Place& p) {
    const double h = PMI_TEXT_MM, side = 1.2 * h;
    const Point2& S = p.S;
    const Point2& o = p.o;
    const Point2 t(o.y(), -o.x());
    const Point2 Ap = S + (side * std::sqrt(3.0) / 2) * o;
    out.poly({S + (side / 2) * t, S - (side / 2) * t, Ap}, true, true);
    const Point2 E = p.free && (p.E - S).norm() > side * std::sqrt(3.0) / 2 + h ? p.E : Point2(Ap + h * o);
    out.line(Ap, E);
    const Point2 C = E + h * o / std::max(std::abs(o.x()), std::abs(o.y()));
    out.poly({C + Point2(-h, -h), C + Point2(h, -h), C + Point2(h, h), C + Point2(-h, h)}, true, false);
    out.text(a.value("letter", std::string()), C, h, "center middle");
    return E;
}

// Roughness of the remaining surfaces in the top right corner of the sheet (§6.18а, FX5-PMI-001): the model of the first
// candidate view of all drawing sheets. Depends on the document only.
const doc::Element* restModel(const doc::Document& d, const std::vector<const doc::Sheet*>& sheets, const doc::Sheet** where,
                              std::string* view) {
    for (const doc::Sheet* sh : sheets) {
        for (const auto& v : sh->views) {
            if (!v.pmi) continue;
            const auto t = viewTriad(*sh, v.id);
            if (!t || !t->base->refFile.empty()) continue;
            const int ei = d.elementIndex(t->base->refElement);
            if (ei < 0) continue;
            const doc::Element& el = d.elements[size_t(ei)];
            if (!partLike(el) || !el.restRoughness) continue;
            *where = sh;
            *view = v.id;
            return &el;
        }
    }
    return nullptr;
}

// Rest roughness marks of the sheet (§6.18а): drawn by the model of the mark view; position — the main sign vertex.
void appendRestRoughness(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, double shift, SheetGraphics& out) {
    for (const auto& m : sheet.marks) {
        if (m.kind != "restRoughness") continue;
        if (sheet.viewIndex(m.view) < 0) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_VIEW_NOT_FOUND").with("view", m.view)});
            continue;
        }
        const auto t = viewTriad(sheet, m.view);
        if (!t) continue;  // the chain error is the view's
        const int ei = t->base->refFile.empty() ? d.elementIndex(t->base->refElement) : -1;
        const doc::Element* model = ei < 0 ? nullptr : &d.elements[size_t(ei)];
        // Set in the drawing (FX12-DRW-007): the values of def, the model's own rest roughness does not act.
        const bool local = m.local && m.def;
        if (!local && (!model || !partLike(*model) || !model->restRoughness)) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_SOURCE_LOST").with("source", t->base->refElement)});
            continue;
        }
        const doc::Json& rr = local ? *m.def : *model->restRoughness;
        // Brackets: the model has roughness marks; set in the drawing — also roughness marks set in the drawing on the sheet.
        const bool brackets =
            (model && partLike(*model) &&
             std::any_of(model->annotations.begin(), model->annotations.end(),
                         [](const doc::Json& a) { return a.value("type", std::string()) == "roughness"; })) ||
            (local && std::any_of(sheet.marks.begin(), sheet.marks.end(), [](const doc::SheetMark& o) {
                 return o.kind == "annotation" && o.local && o.def && o.def->value("type", std::string()) == "roughness";
             }));
        const double h = REST_TEXT_MM;
        const std::string param = pmi::roughnessText(rr);
        const bool shelf = rr.contains("parameter");
        const double tw = widthOf(param, h) / h;
        const double xr = 1.732 + (shelf ? tw + 0.4 : 0);
        const double alpha = std::asin(REST_BRACKET_HALF / REST_BRACKET_R), c = REST_BRACKET_R * std::cos(alpha), g = REST_BRACKET_R - c;
        const double xL = xr + REST_GAP + g, xR = xL + 2 * REST_PAD + 0.577 + 1.732;
        const double xmax = brackets ? xR + g : xr, ytop = brackets ? 1.5 + REST_BRACKET_HALF : 3;
        const double field = drawingStandard(drawing) == font::DrawingStandard::Iso ? ISO_FIELD : 5;
        // Gear tables occupy the corner (§6.20.3): the sign goes left of them.
        const Point2 V = m.position ? Point2((*m.position)[0], (*m.position)[1])
                                    : Point2(out.width - field - shift - REST_MARGIN_MM - h * xmax, out.height - field - REST_MARGIN_MM - h * ytop);
        Out o{"pmi:rest:" + (model ? model->id : m.id), out.primitives};
        out.marks.push_back({m.id, V, {o.role}});
        const size_t firstPrim = out.primitives.size();
        o.glyph(pmi::roughnessGlyph(rr.value("sign", std::string("any")), tw, shelf), V, h);
        o.text(param, V + h * Point2(1.9, 1.8), h, "left bottom");
        if (brackets) {
            o.arc(V + h * Point2(xL + c, 1.5), h * REST_BRACKET_R, PI - alpha, PI + alpha);
            o.glyph(pmi::roughnessGlyph("any", 0, false), V + h * Point2(xL + REST_PAD + 0.577, 0), h);
            o.arc(V + h * Point2(xR - c, 1.5), h * REST_BRACKET_R, -alpha, alpha);
        }
        if (local)  // set in the drawing: the screen color (FX12-DRW-007), outputs ignore it
            for (size_t i = firstPrim; i < out.primitives.size(); ++i) out.primitives[i].local = true;
    }
}

// Drawing sheets in order; the sheet with this sheet's ID is replaced by it (if none — it is the only one).
std::vector<const doc::Sheet*> drawingSheets(const doc::Element& drawing, const doc::Sheet& sheet) {
    std::vector<const doc::Sheet*> sheets;
    for (const auto& sh : drawing.sheets) sheets.push_back(sh.id == sheet.id ? &sheet : &sh);
    if (std::none_of(sheets.begin(), sheets.end(), [&](const doc::Sheet* s) { return s == &sheet; })) sheets = {&sheet};
    return sheets;
}

// Candidate views of a model (§6.18 "Выбор вида") over all sheets.
std::vector<Candidate> candidatesOf(const std::vector<const doc::Sheet*>& sheets, const doc::Element& el) {
    std::vector<Candidate> cands;
    for (const doc::Sheet* sh : sheets)
        for (const auto& v : sh->views) {
            if (v.kind == "detail" || !v.pmi) continue;
            auto t = viewTriad(*sh, v.id);
            if (!t || !t->base->refFile.empty() || t->base->refElement != el.id || t->base->orientation == "iso" || t->sectionOnly)
                continue;
            cands.push_back({sh, &v, *t});
        }
    return cands;
}

// Automatic start and direction on the view (§6.18 "Точка S и направление o"); type — the annotation type.
Place autoPlace(const detail::AnnotationFace& af, const ViewTriad& t, const ViewPlacement& pl, const std::string& type) {
    Classified cl = classify(af, t);
    if (cl.cls == ViewClass::None) cl = {ViewClass::Front, ContourKind::EdgeOn};  // the model changed: as "front"
    auto pi = [&](const Vec3& X) { return Point2(X.dot(t.r), X.dot(t.u)); };
    auto T = [&](const Point2& q) { return Point2(pl.offset + pl.scale * q); };
    const Point2 e45(std::sqrt(0.5), std::sqrt(0.5));
    Place p;
    p.contour = cl.cls == ViewClass::Contour;
    if (!p.contour) {
        p.S = T(pi(af.anchor));
        p.o = type == "datum" ? Point2(0, 1) : e45;
    } else if (cl.kind == ContourKind::EdgeOn) {
        p.S = T(pi(af.anchor));
        const Point2 pn = pi(af.normal);
        p.o = pn.norm() > 0 ? Point2(pn.normalized()) : Point2(0, 1);
    } else {
        const Vec3 ax = af.surface.dir.normalized();
        const Point3 A = af.surface.point;
        const double rho = af.surface.radius;
        if (cl.kind == ContourKind::CylinderEnd) {
            p.S = T(pi(A)) + pl.scale * rho * e45;
            p.o = e45;
        } else {
            const Point3 F = A + (af.anchor - A).dot(ax) * ax;
            const Point2 ah = pi(ax).normalized();
            const Point2 m(-ah.y(), ah.x());
            const double sg = pi(af.anchor - F).dot(m) >= 0 ? 1.0 : -1.0;
            p.S = T(pi(F) + sg * rho * m);
            p.o = af.surface.convex ? Point2(sg * m) : Point2(-sg * m);
        }
    }
    return p;
}

// Image of the annotation face on the view, sheet polylines (§6.25): the face edges and, at a cylinder not seen end-on,
// its two silhouette generatrices.
std::vector<std::vector<Point2>> faceImage(const kernel::Kernel& k, const regen::PartState& S, const detail::AnnotationFace& af,
                                           const ViewTriad& t, const ViewPlacement& pl) {
    std::vector<int> entries;
    for (size_t i = 0; i < S.names.edges.size(); ++i)
        if (S.names.edges[i].f1 == af.entry || S.names.edges[i].f2 == af.entry) entries.push_back(int(i));
    const auto polys = edgePolylines(k, S, entries);
    auto pi = [&](const Vec3& X) { return Point2(X.dot(t.r), X.dot(t.u)); };
    auto T = [&](const Point2& q) { return Point2(pl.offset + pl.scale * q); };
    std::vector<std::vector<Point2>> out;
    for (const auto& pl3 : polys) {
        std::vector<Point2> q;
        for (const auto& P : pl3) q.push_back(T(pi(P)));
        out.push_back(std::move(q));
    }
    if (af.surface.kind == kernel::SurfaceInfo::Kind::Cylinder && !polys.empty()) {
        const Vec3 a = af.surface.dir.normalized();
        if (std::abs(a.dot(t.n)) < std::cos(PMI_EDGE_ON_DEG * PI / 180)) {
            const Point3 A = af.surface.point;
            double tmin = 1e300, tmax = -1e300;
            for (const auto& pl3 : polys)
                for (const auto& P : pl3) {
                    const double v = (P - A).dot(a);
                    tmin = std::min(tmin, v);
                    tmax = std::max(tmax, v);
                }
            const Point2 ah = pi(a).normalized();
            const Point2 m(-ah.y(), ah.x());
            for (double sg : {1.0, -1.0})
                out.push_back({T(pi(A + tmin * a) + sg * af.surface.radius * m), T(pi(A + tmax * a) + sg * af.surface.radius * m)});
        }
    }
    return out;
}

const doc::Json* modelAnnotation(const doc::Element& el, const std::string& id) {
    for (const auto& a : el.annotations)
        if (a.value("id", std::string()) == id) return &a;
    return nullptr;
}

bool drawable(const doc::Json& a) {
    const std::string type = a.value("type", std::string());
    return (type == "roughness" || type == "geometric" || type == "datum") && a.contains("face") && a["face"].is_string();
}

}  // namespace

namespace detail {

std::optional<AnnotationFace> resolveAnnotationFace(const kernel::Kernel& k, const regen::PartState& s, const std::string& faceName) {
    const auto rr = naming::resolve(faceName, naming::EntityKind::Face, s.names);
    if (naming::refFate(rr, true) != naming::RefFate::Resolved) return std::nullopt;
    AnnotationFace af;
    af.entry = rr.entities.front().index;
    const auto& fe = s.names.faces.at(size_t(af.entry));
    const kernel::Shape& shape = s.bodies.at(size_t(fe.body)).shape;
    af.anchor = k.closestPointOnFace(shape, fe.face, k.faceCentroid(shape, fe.face));
    af.normal = k.faceNormalAt(shape, fe.face, af.anchor);
    af.surface = k.faceSurface(shape, fe.face);
    return af;
}

const doc::Element* annotatedModel(const doc::Document& d, const doc::DrawingView& base) {
    if (!base.refFile.empty()) return nullptr;  // models of other files — deferred (§6.18)
    const int ei = d.elementIndex(base.refElement);
    if (ei < 0) return nullptr;
    const doc::Element& el = d.elements[size_t(ei)];
    // Feature dimension tolerances (FX12-PMI-007) are model tolerances too (§6а.3).
    const bool featureTols = std::any_of(el.features.begin(), el.features.end(),
                                         [](const doc::FeaturePtr& f) { return f->tolerances && !f->tolerances->empty(); });
    return annotatable(el) && (!el.annotations.empty() || featureTols) ? &el : nullptr;
}

const doc::Json* holeWallTolerance(const doc::Element& el, const std::string& faceBase) {
    // Hole wall base «F.side(s1/…)» (naming.md §2.3): the tolerance of the hole diameter (pmi.md §2.2, FX12-PMI-007).
    const auto dot = faceBase.find(".side(s1/");
    if (dot == std::string::npos) return nullptr;
    const auto f = el.find(faceBase.substr(0, dot));
    if (!f || f->type != doc::FeatureType::Hole || f->suppressed) return nullptr;
    return doc::featureTolerance(*f, "diameter");
}

std::optional<ModelFit> faceFit(const doc::Element& el, const kernel::Kernel& k, const regen::PartState& s, const std::vector<int>& faces,
                                double radius) {
    // Hole diameter tolerance (FX12-PMI-007): the wall of a hole feature with a diameter tolerance — one source of truth
    // for the hole (before a face designation of an older document), also for dimensions placed by hand on its edges.
    for (int fi : faces) {
        if (fi < 0 || size_t(fi) >= s.names.faces.size()) continue;
        const auto& fe = s.names.faces[size_t(fi)];
        const doc::Json* t = holeWallTolerance(el, fe.base);
        if (!t) continue;
        const auto surf = k.faceSurface(s.bodies.at(size_t(fe.body)).shape, fe.face);
        if (surf.kind != kernel::SurfaceInfo::Kind::Cylinder || std::abs(surf.radius - radius) > DIM_EPS) continue;
        return ModelFit{t, 2 * surf.radius};
    }
    for (const auto& a : el.annotations) {
        if (a.value("type", std::string()) != "fit" || !a.contains("face")) continue;
        const auto af = resolveAnnotationFace(k, s, a["face"].get<std::string>());
        if (!af || std::find(faces.begin(), faces.end(), af->entry) == faces.end()) continue;
        if (af->surface.kind != kernel::SurfaceInfo::Kind::Cylinder || std::abs(af->surface.radius - radius) > DIM_EPS) continue;
        return ModelFit{&a, 2 * af->surface.radius};
    }
    return std::nullopt;
}

std::string toleranceText(const doc::Json& fit, double nominal, const std::optional<std::string>& mode) {
    const bool hasField = fit.contains("field");
    std::optional<pmi::Deviations> dv;
    if (hasField) {
        if (auto r = pmi::fieldDeviations(fit["field"].get<std::string>(), nominal)) dv = *r;
    } else if (fit.contains("upper") && fit.contains("lower")) {
        dv = pmi::Deviations{fit["upper"].get<double>(), fit["lower"].get<double>()};
    }
    // No mode — «field with deviations» (FX13-TOL-004; the former «as in the model» reads so too).
    const std::string m = mode.value_or("fieldDeviations");
    if (m == "hide") return {};
    if (!hasField) return dv ? " " + pmi::deviationsText(*dv) : std::string();  // explicit deviations — only them
    const std::string field = fit["field"].get<std::string>();
    if (m == "field" || !dv) return field;
    if (m == "fieldDeviations") return field + "(" + pmi::deviationsText(*dv) + ")";
    return " " + pmi::deviationsText(*dv);  // deviations
}

}  // namespace detail

void appendAnnotations(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                       SheetGraphics& out) {
    std::map<std::pair<std::string, int>, int> stack;  // (view, face) → number of automatic annotations already placed
    for (const auto& m : sheet.marks) {
        if (m.kind != "annotation") continue;
        if (sheet.viewIndex(m.view) < 0) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_VIEW_NOT_FOUND").with("view", m.view)});
            continue;
        }
        const auto t = viewTriad(sheet, m.view);
        if (!t) continue;  // the chain error is the view's
        if (!sheet.views[size_t(sheet.viewIndex(m.view))].pmi) continue;  // pmi: false — hidden on this view (§6.18)
        const int ei = t->base->refFile.empty() ? d.elementIndex(t->base->refElement) : -1;
        const doc::Element* el = ei < 0 || !annotatable(d.elements[size_t(ei)]) ? nullptr : &d.elements[size_t(ei)];
        const doc::Json* a = m.local ? (m.def ? &*m.def : nullptr) : el ? modelAnnotation(*el, m.source) : nullptr;
        if (!el || (!m.local && (!a || !drawable(*a)))) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_SOURCE_LOST").with("source", m.source)});
            continue;
        }
        if (!a || !drawable(*a)) continue;  // an unsuitable record set in the drawing is not drawn (document.md §2.4г)
        const auto pl = std::find_if(out.placements.begin(), out.placements.end(), [&](const ViewPlacement& p) { return p.view == m.view; });
        if (pl == out.placements.end()) continue;  // view not built (§6.7, §6.8)
        auto st = models.model("", el->id);
        if (!st || !*st) continue;
        const kernel::Kernel& k = models.kernel();
        const regen::PartState& S = **st;
        const auto af = detail::resolveAnnotationFace(k, S, (*a)["face"].get<std::string>());
        if (!af) continue;
        const std::string type = a->value("type", std::string());
        Place p = autoPlace(*af, *t, *pl, type);
        if (m.position) {  // the sign position is given: the arrow to the nearest point of the face image (§6.25)
            const Point2 E((*m.position)[0], (*m.position)[1]);
            const auto near = nearestOnPolylines(faceImage(k, S, *af, *t, *pl), E);
            if (!near) continue;
            p.S = near->first;
            p.free = true;
            p.contour = true;
            if ((E - p.S).norm() >= MARK_MIN_LEADER_MM) {
                p.o = (E - p.S).normalized();
                p.E = E;
            } else {
                p.E = p.S + MARK_MIN_LEADER_MM * p.o;
            }
        } else {
            const int j = stack[{m.view, af->entry}]++;
            p.S += double(j) * PMI_STACK_MM * Point2(p.o.y(), -p.o.x());
            p.E = p.S + PMI_LEADER_MM * p.o;
        }
        Out o2{"view:" + m.view + ":pmi:" + (m.source.empty() ? m.id : m.source), out.primitives};
        Point2 anchor(0, 0);
        if (type == "roughness") anchor = roughness(o2, *a, p);
        else if (type == "geometric") anchor = geometric(o2, *a, p);
        else anchor = datum(o2, *a, p);
        if (m.local)  // set in the drawing (FX9-DRW-009): the screen color
            for (auto& pr : out.primitives)
                if (pr.role == o2.role) pr.local = true;
        out.marks.push_back({m.id, anchor, {o2.role}});
    }
    appendRestRoughness(d, drawing, sheet, gearTablesWidth(drawing, sheet, models), out);
}

namespace {

// The view choice of an annotation with the selected views and the room (§6.18 «Выбранный вид и место», FX14-DRW-005).
struct ViewChooser {
    const doc::Document& d;
    const doc::Element& drawing;
    const doc::Sheet& sheet;  // the sheet of the command
    const ModelProvider& models;
    std::set<std::string> selected;  // selected views of the sheet
    std::map<const doc::Sheet*, std::unique_ptr<MarkRoom>> rooms;  // built lazily, per candidate sheet
    std::vector<doc::SheetMark> placed;                             // placed by the command before (this sheet)
    std::map<size_t, SheetBox> placedBoxes;                         // their boxes, computed on demand

    bool suits(const detail::AnnotationFace& af, const Candidate& c, ViewClass want) const {
        if (c.triad.cutOrigin && (af.anchor - *c.triad.cutOrigin).dot(c.triad.n) > PMI_EPS) return false;
        return classify(af, c.triad).cls == want;
    }

    MarkRoom& roomOf(const doc::Sheet* sh) {
        auto& r = rooms[sh];
        if (!r) r = std::make_unique<MarkRoom>(markRoom(d, drawing, *sh, models));
        return *r;
    }

    // Image box of mark m on its view of sheet sh (automatic placement, stacked after the sheet's annotations of that view
    // and those placed by the command before it); none (view not built, nothing drawn) — nullopt.
    std::optional<SheetBox> imageBox(const doc::Sheet* sh, const doc::SheetMark& m, size_t placedBefore) {
        MarkRoom& room = roomOf(sh);
        doc::Sheet tmp = *sh;
        tmp.marks.clear();
        tmp.dimensions.clear();
        for (const auto& o : sh->marks)
            if (o.kind == "annotation" && o.view == m.view && !o.position) tmp.marks.push_back(o);
        if (sh == &sheet)
            for (size_t i = 0; i < placedBefore && i < placed.size(); ++i)
                if (placed[i].view == m.view) {
                    doc::SheetMark o = placed[i];
                    o.id = "placed:" + std::to_string(i);
                    tmp.marks.push_back(std::move(o));
                }
        doc::SheetMark probe = m;
        probe.id = "probe:";
        probe.position.reset();
        tmp.marks.push_back(std::move(probe));
        SheetGraphics g;
        g.width = room.graphics.width;
        g.height = room.graphics.height;
        g.placements = room.graphics.placements;
        const std::string font = drawingFont(drawing);
        const DrawingFontScope scope(font);
        appendAnnotations(d, drawing, tmp, models, g);
        const auto it = std::find_if(g.marks.begin(), g.marks.end(), [](const MarkPlacement& p) { return p.mark == "probe:"; });
        if (it == g.marks.end()) return std::nullopt;
        std::vector<Primitive> prims;
        for (const auto& p : g.primitives)
            if (std::find(it->roles.begin(), it->roles.end(), p.role) != it->roles.end()) prims.push_back(p);
        if (prims.empty()) return std::nullopt;
        applyDrawingFont(prims, font);
        return primitivesExtent(prims);
    }

    int interference(const Candidate& c, const doc::SheetMark& m) {
        const auto box = imageBox(c.sheet, m, placed.size());
        if (!box) return 1;  // not drawn there — as a mark without room
        std::vector<SheetBox> extra;
        if (c.sheet == &sheet)
            for (size_t i = 0; i < placed.size(); ++i) {
                auto it = placedBoxes.find(i);
                if (it == placedBoxes.end()) {
                    const auto b = imageBox(&sheet, placed[i], i);
                    it = placedBoxes.emplace(i, b.value_or(SheetBox{})).first;
                }
                if (!it->second.empty()) extra.push_back(it->second);
            }
        return markInterference(roomOf(c.sheet), *box, c.view->id, extra);
    }

    const Candidate* choose(const detail::AnnotationFace& af, const std::vector<Candidate>& cands, const doc::SheetMark& m) {
        // 1. The selected views: "contour", then "front"; no room check.
        if (!selected.empty())
            for (ViewClass want : {ViewClass::Contour, ViewClass::Front})
                for (const auto& c : cands)
                    if (c.sheet == &sheet && selected.count(c.view->id) && suits(af, c, want)) return &c;
        // 2. All "contour", then all "front": the first that fits, else the fewest interferences.
        std::vector<const Candidate*> order;
        for (ViewClass want : {ViewClass::Contour, ViewClass::Front})
            for (const auto& c : cands)
                if (suits(af, c, want)) order.push_back(&c);
        if (order.size() <= 1) return order.empty() ? nullptr : order.front();
        const Candidate* best = nullptr;
        int bestN = 0;
        for (const Candidate* c : order) {
            doc::SheetMark probe = m;
            probe.view = c->view->id;
            const int n = interference(*c, probe);
            if (n == 0) return c;
            if (!best || n < bestN) best = c, bestN = n;
        }
        return best;
    }
};

}  // namespace

std::vector<doc::SheetMark> annotationMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                            const ModelProvider& models, const std::vector<std::string>& views) {
    std::vector<doc::SheetMark> out;
    const auto sheets = drawingSheets(drawing, sheet);
    ViewChooser chooser{d, drawing, sheet, models, {}, {}, {}, {}};
    for (const auto& v : views)
        if (sheet.viewIndex(v) >= 0) chooser.selected.insert(v);
    // Models with annotations — in order of first appearance among the sheet views.
    std::vector<const doc::Element*> elems;
    for (const auto& v : sheet.views) {
        const auto t = viewTriad(sheet, v.id);
        if (!t) continue;
        const doc::Element* el = detail::annotatedModel(d, *t->base);
        if (el && std::find(elems.begin(), elems.end(), el) == elems.end()) elems.push_back(el);
    }
    for (const doc::Element* el : elems) {
        auto st = models.model("", el->id);
        if (!st || !*st) continue;
        const kernel::Kernel& k = models.kernel();
        const auto cands = candidatesOf(sheets, *el);
        for (const auto& a : el->annotations) {
            if (!drawable(a)) continue;
            const auto af = detail::resolveAnnotationFace(k, **st, a["face"].get<std::string>());
            if (!af) continue;
            doc::SheetMark m;
            m.kind = "annotation";
            m.source = a.value("id", std::string());
            const Candidate* c = chooser.choose(*af, cands, m);
            if (!c || c->sheet != &sheet) continue;
            m.view = c->view->id;
            chooser.placed.push_back(m);
            out.push_back(std::move(m));
        }
    }
    const doc::Sheet* where = nullptr;
    std::string view;
    if (restModel(d, sheets, &where, &view) && where == &sheet) {
        doc::SheetMark m;
        m.kind = "restRoughness";
        m.view = view;
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace construe::drawing
