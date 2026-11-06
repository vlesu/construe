// Assembly weld seams on drawings (spec/drawing.md §6.19) and their status parts (spec/weld.md §6).
#include "drawing/weld_annotations.h"

#include "drawing/render.h"
#include "drawing/sheet_marks.h"
#include "drawing/view_parts.h"
#include "drawing/weld_geometry.h"
#include "regen/assembly_ref.h"
#include "regen/naming.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace construe::drawing {

// ---------------------------------------------------------------- values and edges (weld.md §6)

WeldValues weldValues(const doc::Document& d, const doc::Json& weld) {
    WeldValues out;
    const auto vars = regen::documentVariables(d);
    const auto units = regen::documentUnits(d);
    auto eval = [&](const doc::Json* text, const char* name, std::optional<double>& into) {
        if (!text || !text->is_string() || out.error) return;
        auto q = expr::evaluateField(text->get<std::string>(), expr::LENGTH, vars.env(), units);
        if (!q) {
            out.error = Error("WELD_BAD_VALUE").with("name", std::string(name));
            return;
        }
        into = q->value;
    };
    if (weld.contains("leg")) eval(&weld["leg"], "leg", out.leg);
    if (weld.contains("intermittent")) {
        const auto& im = weld["intermittent"];
        eval(im.contains("length") ? &im["length"] : nullptr, "length", out.length);
        eval(im.contains("pitch") ? &im["pitch"] : nullptr, "pitch", out.pitch);
    }
    if (out.error) return out;
    if (out.leg && *out.leg <= 0) out.error = Error("WELD_BAD_VALUE").with("name", std::string("leg"));
    else if (out.length && *out.length <= 0) out.error = Error("WELD_BAD_VALUE").with("name", std::string("length"));
    else if (out.pitch && out.length && *out.pitch <= *out.length) out.error = Error("WELD_BAD_VALUE").with("name", std::string("pitch"));
    return out;
}

pmi::WeldTexts weldTexts(const WeldValues& v) {
    pmi::WeldTexts t;
    if (v.leg) t.leg = pmi::formatNumber(*v.leg, pmi::WELD_DECIMALS);
    if (v.length) t.length = pmi::formatNumber(*v.length, pmi::WELD_DECIMALS);
    if (v.pitch) t.pitch = pmi::formatNumber(*v.pitch, pmi::WELD_DECIMALS);
    return t;
}

namespace {

// Resolved entries of an edge reference: EXACT, REMAPPED — the entity, SPLIT — all parts (weld.md §6).
std::vector<int> resolvedEdges(const naming::NameTable& names, const std::string& ref) {
    const auto rr = naming::resolve(ref, naming::EntityKind::Edge, names);
    if (rr.error || rr.entities.empty()) return {};
    if (rr.status != naming::ResolveStatus::Exact && rr.status != naming::ResolveStatus::Remapped && rr.status != naming::ResolveStatus::Split)
        return {};
    std::vector<int> out;
    for (const auto& e : rr.entities) out.push_back(e.index);
    return out;
}

// Orthogonal pair (e1, e2) to unit n (assembly.md §7.4): e = the basis vector least parallel to n.
std::pair<Vec3, Vec3> orthoPair(const Vec3& n) {
    int i = 0;
    if (std::abs(n.y()) < std::abs(n[i])) i = 1;
    if (std::abs(n.z()) < std::abs(n[i])) i = 2;
    const Vec3 u = n.cross(Vec3::Unit(i)).normalized();
    return {u, n.cross(u)};
}

}  // namespace

std::optional<std::string> weldLostEdge(const naming::NameTable& names, const doc::Json& weld) {
    if (!weld.contains("edges")) return std::nullopt;
    for (const auto& e : weld["edges"])
        if (resolvedEdges(names, e.get<std::string>()).empty()) return e.get<std::string>();
    return std::nullopt;
}

std::vector<std::vector<Point3>> edgePolylines(const kernel::Kernel& k, const regen::PartState& s, const std::vector<int>& entries) {
    std::vector<std::vector<Point3>> out;
    std::map<int, kernel::CanonicalTopology> topo;
    std::map<int, kernel::Tessellation> tess;
    const double step = 2 * std::numbers::pi / WELD_CIRCLE_SEGMENTS;
    for (int idx : entries) {
        const auto& ee = s.names.edges.at(size_t(idx));
        const kernel::Shape& shape = s.bodies.at(size_t(ee.body)).shape;
        const auto ci = k.edgeCurve(shape, ee.logical);
        std::vector<Point3> pl;
        if (ci.kind == kernel::CurveInfo::Kind::Line) {
            pl = {ci.p1, ci.p2};
        } else if (ci.kind == kernel::CurveInfo::Kind::Circle) {
            const Vec3 n = ci.axis.normalized();
            const auto [e1, e2] = orthoPair(n);
            auto at = [&](double phi) { return Point3(ci.center + ci.radius * (std::cos(phi) * e1 + std::sin(phi) * e2)); };
            auto angleOf = [&](const Point3& p) { return std::atan2((p - ci.center).dot(e2), (p - ci.center).dot(e1)); };
            if (ci.closed) {
                for (int i = 0; i <= WELD_CIRCLE_SEGMENTS; ++i) pl.push_back(at(step * i));
            } else {
                if (!topo.count(ee.body)) topo[ee.body] = kernel::canonicalTopology(k, shape);
                const auto& ends = topo[ee.body].logicalEdges.at(size_t(ee.logical)).ends;
                if (ends.size() == 2) {
                    const Point3 A = k.vertexPoint(shape, ends[0]), B = k.vertexPoint(shape, ends[1]);
                    const double a0 = angleOf(A);
                    double sweep = geom2::ccwSweep(a0, angleOf(B));
                    if (sweep == 0) sweep = 2 * std::numbers::pi;
                    if (geom2::ccwSweep(a0, angleOf(ee.key)) > sweep) sweep -= 2 * std::numbers::pi;  // the key is on the other way
                    const int n = std::max(1, int(std::ceil(std::abs(sweep) / step - 1e-9)));
                    for (int i = 0; i <= n; ++i) pl.push_back(at(a0 + sweep * i / n));
                    pl.front() = A;
                    pl.back() = B;
                }
            }
        }
        if (pl.empty()) {  // another curve: polylines of the logical edge of the body tessellation
            if (!tess.count(ee.body)) {
                auto t = k.tessellate(shape, {WELD_TESS_CHORD, 0.2});
                tess[ee.body] = t ? std::move(*t) : kernel::Tessellation{};
            }
            for (const auto& em : tess[ee.body].edges) {
                if (em.edge != ee.logical) continue;
                for (const auto& f : em.polylines) {
                    std::vector<Point3> q;
                    for (size_t i = 0; i + 2 < f.size(); i += 3) q.emplace_back(f[i], f[i + 1], f[i + 2]);
                    if (q.size() >= 2) out.push_back(std::move(q));
                }
            }
            continue;
        }
        out.push_back(std::move(pl));
    }
    return out;
}

std::vector<std::vector<Point3>> weldPolylines(const kernel::Kernel& k, const regen::PartState& s, const doc::Json& weld) {
    std::vector<std::vector<Point3>> out;
    if (weld.contains("faces")) {  // the weld line of the faces (weld.md §6а)
        for (auto& p : weldPath(k, s, weld).pieces) out.push_back(std::move(p.points));
        return out;
    }
    if (!weld.contains("edges")) return out;
    std::vector<int> entries;
    for (const auto& ref : weld["edges"])
        for (int idx : resolvedEdges(s.names, ref.get<std::string>())) entries.push_back(idx);
    return edgePolylines(k, s, entries);
}

// ---------------------------------------------------------------- sheet (drawing.md §6.19)

namespace {

double curveDist(const Point2& p, const kernel::ViewCurve& c) {
    switch (c.kind) {
        case kernel::ViewCurve::Kind::Line: return geom2::distToSegment(p, c.p1, c.p2);
        case kernel::ViewCurve::Kind::Circle: return std::abs((p - c.center).norm() - c.radius);
        case kernel::ViewCurve::Kind::Arc: {
            const double a = std::atan2(p.y() - c.center.y(), p.x() - c.center.x());
            if (geom2::ccwSweep(c.start, a) <= c.end - c.start) return std::abs((p - c.center).norm() - c.radius);
            const Point2 s = c.center + c.radius * Point2(std::cos(c.start), std::sin(c.start));
            const Point2 e = c.center + c.radius * Point2(std::cos(c.end), std::sin(c.end));
            return std::min((p - s).norm(), (p - e).norm());
        }
        case kernel::ViewCurve::Kind::Polyline: {
            double best = 1e300;
            for (size_t i = 0; i + 1 < c.points.size(); ++i) best = std::min(best, geom2::distToSegment(p, c.points[i], c.points[i + 1]));
            return best;
        }
    }
    return 1e300;
}

bool nearCurve(const Point2& p, const std::vector<kernel::ViewCurve>& curves, double tol) {
    return std::any_of(curves.begin(), curves.end(), [&](const kernel::ViewCurve& c) { return curveDist(p, c) <= tol; });
}

struct Run {
    bool visible = true;
    std::vector<Point2> points;  // sheet
    double length() const {
        double l = 0;
        for (size_t i = 0; i + 1 < points.size(); ++i) l += (points[i + 1] - points[i]).norm();
        return l;
    }
};

// Runs of the weld image on a view (§6.19 "Изображение шва"): projection, division, visibility classes.
std::vector<Run> imageRuns(const std::vector<std::vector<Point3>>& polys, const ViewMapping& m, const kernel::HlrProjection& pr) {
    std::vector<Run> runs;
    const double step = WELD_SAMPLE_MM / m.scale, tol = WELD_MATCH_MM / m.scale;
    for (const auto& pl : polys) {
        Run cur;
        bool open = false;
        for (size_t i = 0; i + 1 < pl.size(); ++i) {
            const Point2 a = m.toView(pl[i]), b = m.toView(pl[i + 1]);
            const double len = (b - a).norm();
            if (len < 1e-12) continue;
            const int n = std::max(1, int(std::ceil(len / step - 1e-9)));
            for (int j = 0; j < n; ++j) {
                const Point2 p0 = a + (b - a) * (double(j) / n), p1 = a + (b - a) * (double(j + 1) / n);
                const Point2 mid = (p0 + p1) / 2;
                bool vis = true;
                if (!nearCurve(mid, pr.visible, tol) && !nearCurve(mid, pr.visibleSmooth, tol) && (nearCurve(mid, pr.hidden, tol) || nearCurve(mid, pr.hiddenSmooth, tol)))
                    vis = false;
                if (!open || cur.visible != vis) {
                    if (open) runs.push_back(std::move(cur));
                    cur = Run{vis, {m.toSheet(p0)}};
                    open = true;
                }
                cur.points.push_back(m.toSheet(p1));
            }
        }
        if (open) runs.push_back(std::move(cur));
    }
    return runs;
}

Primitive polylinePrim(std::vector<Point2> pts, LineType lt, Weight w, const std::string& role, bool closed = false, bool filled = false) {
    Primitive p;
    p.kind = PrimitiveKind::Polyline;
    p.points = std::move(pts);
    p.closed = closed;
    p.filled = filled;
    p.lineType = lt;
    p.weight = w;
    p.role = role;
    return p;
}

void appendGlyph(const pmi::Glyph& g, const Point2& origin, double h, const std::string& role, std::vector<Primitive>& out) {
    for (const auto& pl : g.polylines) {
        std::vector<Point2> pts;
        for (const auto& q : pl) pts.push_back(origin + h * q);
        out.push_back(polylinePrim(std::move(pts), LineType::Solid, Weight::Thin, role));
    }
    for (const auto& c : g.circles) {
        Primitive p;
        p.kind = PrimitiveKind::Circle;
        p.center = origin + h * c.center;
        p.radius = h * c.r;
        p.role = role;
        out.push_back(std::move(p));
    }
    for (const auto& f : g.filled) {
        std::vector<Point2> pts;
        for (const auto& q : f) pts.push_back(origin + h * q);
        out.push_back(polylinePrim(std::move(pts), LineType::Solid, Weight::Thin, role, true, true));
    }
}

size_t codePoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Designation (§6.19 "Выноска", "Надпись"; §6.25 — the given position E): arrow, leader, shelf, break signs, label.
// fromHidden — the leader starts at the invisible (dashed) image: the weld is seen from its reverse side (FX8-WLD-003).
void appendDesignation(const doc::Json& w, const pmi::WeldTexts& texts, const Point2& S, const Point2& E, bool fromHidden,
                       const std::string& role, std::vector<Primitive>& out) {
    const double h = WELD_TEXT_MM;
    const Point2 o = (E - S).norm() > 0 ? Point2((E - S).normalized()) : Point2(std::cos(std::numbers::pi / 4), std::sin(std::numbers::pi / 4));
    const Point2 t(-o.y(), o.x());
    // Below the shelf — the leader from the reverse side (GOST 2.312): the invisible image or side = back.
    const bool back = fromHidden || w.value("side", std::string()) == "back";
    const Point2 B = S + ARROW_HEAD_LEN_MM * o;
    out.push_back(polylinePrim({S, B, B + WELD_ARROW_HALF_MM * t}, LineType::Solid, Weight::Thin, role, true, true));
    out.push_back(detail::makeLine(S, E, LineType::Solid, Weight::Thin, role));
    // Label width: items left to right.
    const auto items = pmi::weldLabel(w, texts);
    double width = 0;
    for (const auto& it : items) {
        if (it.kind == pmi::WeldLabelItem::Kind::Text) {
            auto tw = textWidth(it.text, h);
            width += tw ? *tw : 0.7 * h * double(codePoints(it.text));
        } else {
            width += pmi::weldGlyph(it.glyph).advance * h;
        }
    }
    // The shelf goes to the right of E, or to the left when E is left of S (§6.25).
    const bool left = E.x() < S.x();
    const double x0 = left ? E.x() - WELD_TEXT_GAP_MM - width : E.x() + WELD_TEXT_GAP_MM;
    std::vector<Primitive> label;
    double x = x0;
    for (const auto& it : items) {
        if (it.kind == pmi::WeldLabelItem::Kind::Text) {
            const Point2 at(x, back ? E.y() - WELD_TEXT_GAP_MM : E.y() + WELD_TEXT_GAP_MM);
            label.push_back(detail::makeText(at, it.text, h, back ? "left top" : "left bottom", role));
            auto tw = textWidth(it.text, h);
            x += tw ? *tw : 0.7 * h * double(codePoints(it.text));
        } else {
            const auto g = pmi::weldGlyph(it.glyph);
            appendGlyph(g.glyph, Point2(x, back ? E.y() - WELD_TEXT_GAP_MM - h : E.y() + WELD_TEXT_GAP_MM), h, role, label);
            x += g.advance * h;
        }
    }
    if (left) out.push_back(detail::makeLine(Point2(x0 - WELD_TEXT_GAP_MM, E.y()), E, LineType::Solid, Weight::Thin, role));
    else out.push_back(detail::makeLine(E, Point2(x + WELD_TEXT_GAP_MM, E.y()), LineType::Solid, Weight::Thin, role));
    const bool around = w.value("allAround", false);
    if (around) {
        Primitive c;
        c.kind = PrimitiveKind::Circle;
        c.center = E;
        c.radius = pmi::WELD_CIRCLE_R * h;
        c.role = role;
        out.push_back(std::move(c));
    }
    if (w.value("field", false)) appendGlyph(pmi::weldFlagGlyph(around), E, h, role, out);
    for (auto& p : label) out.push_back(std::move(p));
}

bool imageView(const doc::DrawingView& v) { return v.kind == "base" || v.kind == "projected"; }

// Top-level instance of an edge or face reference is hidden or suppressed — the reference is not shown (weld.md §6).
doc::Json shownEdges(const doc::Element& asmEl, const doc::Json& w) {
    doc::Json out = w;
    for (const char* key : {"edges", "faces"}) {
        if (!w.contains(key)) continue;
        out[key] = doc::Json::array();
        for (const auto& e : w[key]) {
            const std::string ref = e.get<std::string>();
            const std::string top = regen::refTopInstance(ref);  // assembly.md §3.1
            bool shown = true;
            for (const auto& in : asmEl.instances)
                if (in.id == top && (in.hidden || in.suppressed)) shown = false;
            if (shown) out[key].push_back(e);
        }
    }
    return out;
}

}  // namespace

namespace {

// View of a weld designation (§6.19 "Вид обозначения") over all drawing sheets; none — {nullptr, ""}.
std::pair<const doc::Sheet*, std::string> weldView(const doc::Element& drawing, const doc::Sheet& sheet, const doc::Element& el,
                                                   const std::vector<std::vector<Point3>>& polys) {
    double L3 = 0;
    for (const auto& pl : polys)
        for (size_t i = 0; i + 1 < pl.size(); ++i) L3 += (pl[i + 1] - pl[i]).norm();
    const doc::Sheet* bestSheet = nullptr;
    std::string bestView;
    double bestL = 0;
    for (const auto& sh0 : drawing.sheets) {
        const doc::Sheet& sh = sh0.id == sheet.id ? sheet : sh0;
        for (const auto& v : sh.views) {
            if (!imageView(v)) continue;
            auto tr = viewTriad(sh, v.id);
            if (!tr || !tr->base->refFile.empty() || tr->base->refElement != el.id || tr->base->exploded) continue;
            double L = 0;
            for (const auto& pl : polys)
                for (size_t i = 0; i + 1 < pl.size(); ++i) {
                    const Vec3 dv = pl[i + 1] - pl[i];
                    L += Point2(dv.dot(tr->r), dv.dot(tr->u)).norm();
                }
            if (L >= WELD_FACING_RATIO * L3 && L > 0) return {&sh, v.id};
            if (L > bestL) bestSheet = &sh, bestView = v.id, bestL = L;
        }
    }
    return {bestSheet, bestView};
}

}  // namespace

void appendWelds(const doc::Document& d, const doc::Element& /*drawing*/, const doc::Sheet& sheet, const ModelProvider& models,
                 SheetGraphics& out) {
    // Weld marks of the sheet in order (§6.19, §6.25).
    struct Item {
        const doc::SheetMark* mark;
        const doc::Element* el;
        const doc::Json* weld;
        pmi::WeldTexts texts;
        std::vector<Run> runs;
    };
    std::vector<Item> items;
    for (const auto& m : sheet.marks) {
        if (m.kind != "weld") continue;
        const int vi = sheet.viewIndex(m.view);
        if (vi < 0) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_VIEW_NOT_FOUND").with("view", m.view)});
            continue;
        }
        auto tr = viewTriad(sheet, m.view);
        if (!tr) continue;  // the chain error is the view's
        const int ei = tr->base->refFile.empty() ? d.elementIndex(tr->base->refElement) : -1;
        const doc::Element* el = ei < 0 || !d.elements[size_t(ei)].isAssembly() ? nullptr : &d.elements[size_t(ei)];
        const doc::Json* weld = nullptr;
        if (el)
            for (const auto& w : el->welds)
                if (w.value("id", std::string()) == m.source) weld = &w;
        if (!weld) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_SOURCE_LOST").with("source", m.source)});
            continue;
        }
        const auto values = weldValues(d, *weld);
        if (values.error || !imageView(sheet.views[size_t(vi)]) || tr->base->exploded) continue;  // §6.24
        if (std::none_of(out.placements.begin(), out.placements.end(), [&](const ViewPlacement& p) { return p.view == m.view; })) continue;
        items.push_back({&m, el, weld, weldTexts(values), {}});
    }
    if (items.empty()) return;  // the kernel is requested only when there is something to draw
    const kernel::Kernel& k = models.kernel();
    std::map<std::string, std::shared_ptr<const kernel::HlrProjection>> projections;  // view → projection
    for (auto& it : items) {
        auto st = models.model("", it.el->id);
        if (!st || !*st) continue;
        auto bodies = models.modelBodies("", it.el->id);
        auto m = viewMapping(sheet, it.mark->view, models);
        if (!bodies || !m) continue;
        auto& pr = projections[it.mark->view];
        if (!pr) {
            if (bodies->empty()) {
                pr = std::make_shared<const kernel::HlrProjection>();
            } else {
                const double chordTol = std::clamp(DRAWING_CURVE_TOL / m->scale, kernel::KERNEL_MIN_CHORD, kernel::KERNEL_MAX_CHORD);
                auto r = models.projection(*bodies, kernel::ViewDirection{m->n, m->u}, {chordTol});
                if (!r) continue;
                pr = *r;
            }
        }
        it.runs = imageRuns(weldPolylines(k, **st, shownEdges(*it.el, *it.weld)), *m, *pr);
    }
    // Images: marks in order, inside — edges, polylines.
    for (const auto& it : items) {
        const std::string role = "view:" + it.mark->view + ":weld:" + it.mark->source;
        for (const auto& r : it.runs)
            out.primitives.push_back(polylinePrim(r.points, r.visible ? LineType::Solid : LineType::Dashed, r.visible ? Weight::Thick : Weight::Thin, role));
    }
    // Designations.
    for (const auto& it : items) {
        if (it.runs.empty()) continue;
        const Point2 o45(std::cos(std::numbers::pi / 4), std::sin(std::numbers::pi / 4));
        Point2 S(0, 0), E(0, 0);
        bool hidden = false;
        if (it.mark->position) {  // the arrow to the nearest point of the weld image (§6.25)
            E = Point2((*it.mark->position)[0], (*it.mark->position)[1]);
            std::vector<std::vector<Point2>> polys;
            for (const auto& r : it.runs) polys.push_back(r.points);
            const auto near = nearestOnPolylines(polys, E);
            if (!near) continue;
            S = near->first;
            hidden = !it.runs[near->second].visible;
            if ((E - S).norm() < MARK_MIN_LEADER_MM) E = S + MARK_MIN_LEADER_MM * o45;
        } else {
            const Run* best = nullptr;
            for (bool vis : {true, false}) {
                for (const auto& r : it.runs)
                    if (r.visible == vis && (!best || r.length() > best->length())) best = &r;
                if (best) break;
            }
            // Middle of the run by length.
            const double half = best->length() / 2;
            S = best->points.front();
            double acc = 0;
            for (size_t i = 0; i + 1 < best->points.size(); ++i) {
                const double l = (best->points[i + 1] - best->points[i]).norm();
                if (acc + l >= half && l > 0) {
                    S = best->points[i] + (best->points[i + 1] - best->points[i]) * ((half - acc) / l);
                    break;
                }
                acc += l;
            }
            hidden = !best->visible;
            E = S + WELD_LEADER_MM * o45;
        }
        const std::string role = "view:" + it.mark->view + ":weldmark:" + it.mark->source;
        appendDesignation(*it.weld, it.texts, S, E, hidden, role, out.primitives);
        out.marks.push_back({it.mark->id, E, {role, "view:" + it.mark->view + ":weld:" + it.mark->source}});
    }
}

std::vector<doc::SheetMark> weldMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                      const ModelProvider& models) {
    std::vector<doc::SheetMark> out;
    // Assemblies of this document with welds, in the order of first appearance among the sheet views.
    std::vector<const doc::Element*> asms;
    for (const auto& v : sheet.views) {
        auto tr = viewTriad(sheet, v.id);
        if (!tr || !tr->base->refFile.empty()) continue;
        const int ei = d.elementIndex(tr->base->refElement);
        if (ei < 0) continue;
        const doc::Element* el = &d.elements[size_t(ei)];
        if (el->isAssembly() && !el->welds.empty() && std::find(asms.begin(), asms.end(), el) == asms.end()) asms.push_back(el);
    }
    if (asms.empty()) return out;
    const kernel::Kernel& k = models.kernel();
    for (const doc::Element* el : asms) {
        auto st = models.model("", el->id);
        if (!st || !*st) continue;
        for (const auto& w : el->welds) {
            if (weldValues(d, w).error) continue;
            const auto polys = weldPolylines(k, **st, shownEdges(*el, w));
            if (polys.empty()) continue;
            const auto [where, view] = weldView(drawing, sheet, *el, polys);
            if (where != &sheet) continue;
            doc::SheetMark m;
            m.kind = "weld";
            m.view = view;
            m.source = w.value("id", std::string());
            out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace construe::drawing
