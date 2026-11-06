// Item numbers (balloons) and parts lists on assembly drawing sheets (spec/drawing.md §6.27, §6.28; GOST 2.109,
// GOST 2.316; FX14-DRW-002, FX14-DRW-003): the numbers and rows come from the specification of the view's assembly.
#include "drawing/sheet_parts.h"

#include "drawing/dimensions.h"
#include "drawing/render.h"
#include "drawing/sheet_marks.h"
#include "drawing/view_pmi.h"
#include "base/constants.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace construe::drawing {

namespace {

constexpr double PARTS_LIST_RIGHT_MARGIN_MM = 5;  // the frame line of the GOST frame (§5.2)

const doc::Element* assemblyOfThisDocument(const doc::Document& d, const std::string& element) {
    const int ei = d.elementIndex(element);
    if (ei < 0 || !d.elements[size_t(ei)].isAssembly()) return nullptr;
    return &d.elements[size_t(ei)];
}

// Drawing model (§5.6): the element of the first main view of the first sheet referencing this file; empty — none.
std::string drawingModel(const doc::Element& drawing) {
    if (drawing.sheets.empty()) return {};
    for (const auto& v : drawing.sheets.front().views) {
        if (v.kind != "base" || v.refElement.empty()) continue;
        return v.refFile.empty() ? v.refElement : std::string();
    }
    return {};
}

double textW(const std::string& t, double h) {
    if (auto w = textWidth(t, h)) return *w;
    size_t chars = 0;
    for (unsigned char ch : t) chars += (ch & 0xC0) != 0x80;
    return 0.7 * h * double(chars);
}

Primitive line(const Point2& a, const Point2& b, const std::string& role) {
    Primitive p;
    p.kind = PrimitiveKind::Line;
    p.points = {a, b};
    p.weight = Weight::Thin;
    p.role = role;
    return p;
}

}  // namespace

SpecContent assemblySpecContent(const doc::Document& d, const std::string& assembly, const ModelProvider& models) {
    const doc::Element* spec = nullptr;
    for (const auto& el : d.elements)
        if (el.isSpecification() && el.specAssembly == assembly) {
            spec = &el;
            break;
        }
    doc::Element plain;  // no specification of the assembly — one without overrides and manual rows
    plain.kind = "specification";
    plain.specAssembly = assembly;
    regen::StatePtr st;
    if (auto r = models.model("", assembly); r && *r) st = *r;
    const LinkedDocs linked = [&models](const std::string& file) { return models.linkedDocument(file); };
    return specificationContent(d, spec ? *spec : plain, linked, st ? &st->connections : nullptr);
}

std::optional<size_t> balloonItem(const doc::Element& assembly, const SpecContent& c, const std::string& face) {
    std::string top = face.substr(0, face.find('/'));
    for (const auto& p : assembly.patterns)  // a child of an instance pattern — the row of its source (assembly.md §7е)
        if (p.id == top) top = p.source();
    for (size_t i = 0; i < c.items.size(); ++i)
        for (const auto& x : c.items[i].instances)
            // A fastener of a connection (FX14-SPEC-001): the derived instance "<feature>/<output>" — a prefix of the path.
            if (x == top || (x.find('/') != std::string::npos && face.rfind(x + "/", 0) == 0)) return i;
    return std::nullopt;
}

std::optional<std::string> balloonNumber(const doc::Element& assembly, const SpecContent& c, const std::string& face) {
    const auto i = balloonItem(assembly, c, face);
    if (!i || c.items[*i].position.empty()) return std::nullopt;
    return c.items[*i].position;
}

std::vector<doc::SheetMark> balloonMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                         const ModelProvider& models, const std::vector<std::string>& views) {
    std::vector<doc::SheetMark> out;
    // Assemblies of this document with views on the sheet, in order of first appearance; their views in sheet order.
    std::vector<std::pair<const doc::Element*, std::vector<const doc::DrawingView*>>> asms;
    for (const auto& v : sheet.views) {
        const auto t = viewTriad(sheet, v.id);
        if (!t || !t->base->refFile.empty()) continue;
        const doc::Element* A = assemblyOfThisDocument(d, t->base->refElement);
        if (!A) continue;
        auto it = std::find_if(asms.begin(), asms.end(), [&](const auto& a) { return a.first == A; });
        if (it == asms.end()) {
            asms.emplace_back(A, std::vector<const doc::DrawingView*>{});
            it = std::prev(asms.end());
        }
        it->second.push_back(&v);
    }
    for (const auto& entry : asms) {
        const doc::Element* A = entry.first;  // no structured bindings: they are captured by the lambdas below
        const auto& avs = entry.second;
        auto st = models.model("", A->id);
        if (!st || !*st) continue;  // the model is not computed — nothing to show yet
        const SpecContent c = assemblySpecContent(d, A->id, models);
        // Rows that already have a balloon on any sheet of the drawing (this sheet — as passed).
        std::vector<bool> taken(c.items.size(), false);
        auto mark = [&](const doc::Sheet& s) {
            for (const auto& m : s.marks) {
                if (m.kind != "balloon") continue;
                const auto t = viewTriad(s, m.view);
                if (!t || !t->base->refFile.empty() || t->base->refElement != A->id) continue;
                if (const auto i = balloonItem(*A, c, m.source)) taken[*i] = true;
            }
        };
        bool sheetSeen = false;
        for (const auto& sh : drawing.sheets) {
            sheetSeen = sheetSeen || sh.id == sheet.id;
            mark(sh.id == sheet.id ? sheet : sh);
        }
        if (!sheetSeen) mark(sheet);
        // The view order (FX14-DRW-004, as FX14-DRW-001): selected, the main views, the others.
        std::vector<const doc::DrawingView*> order;
        auto push = [&](const doc::DrawingView* v) {
            if (std::find(order.begin(), order.end(), v) == order.end()) order.push_back(v);
        };
        for (const auto* v : avs)
            if (std::find(views.begin(), views.end(), v->id) != views.end()) push(v);
        for (const auto* v : avs)
            if (v->kind == "base") push(v);
        for (const auto* v : avs) push(v);
        // Per view: the face of each row drawn on it — the line of the part nearest to the viewer (the first at a tie).
        std::map<std::string, std::map<size_t, std::pair<double, std::string>>> faces;  // view → row → (depth, face)
        for (const auto* v : order) {
            auto& f = faces[v->id];
            const auto es = viewEntities(d, drawing, sheet, v->id, models);
            if (!es) continue;
            for (const auto& e : *es) {
                if (e.kind == ViewEntity::Kind::Point || e.face.empty() || e.ref == "axis" || e.ref == "center") continue;
                const auto i = balloonItem(*A, c, e.face);
                if (!i || taken[*i] || c.items[*i].position.empty()) continue;
                const auto it = f.find(*i);
                if (it == f.end() || e.depth > it->second.first) f[*i] = {e.depth, e.face};
            }
        }
        for (size_t i = 0; i < c.items.size(); ++i) {
            if (taken[i]) continue;
            for (const auto* v : order) {
                const auto it = faces[v->id].find(i);
                if (it == faces[v->id].end()) continue;
                doc::SheetMark m;
                m.kind = "balloon";
                m.view = v->id;
                m.source = it->second.second;
                out.push_back(std::move(m));
                break;
            }
        }
    }
    return out;
}

void appendBalloons(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                    SheetGraphics& out) {
    (void)drawing;
    std::map<std::string, SpecContent> specs;  // assembly → its specification (once per build)
    for (const auto& m : sheet.marks) {
        if (m.kind != "balloon") continue;
        if (sheet.viewIndex(m.view) < 0) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_VIEW_NOT_FOUND").with("view", m.view)});
            continue;
        }
        const auto t = viewTriad(sheet, m.view);
        if (!t) continue;  // the chain error is the view's
        const doc::Element* A = t->base->refFile.empty() ? assemblyOfThisDocument(d, t->base->refElement) : nullptr;
        auto lost = [&] { out.errors.push_back({m.id, Error("DRAWING_MARK_SOURCE_LOST").with("source", m.source)}); };
        if (!A) {
            lost();
            continue;
        }
        const auto pl = std::find_if(out.placements.begin(), out.placements.end(), [&](const ViewPlacement& p) { return p.view == m.view; });
        if (pl == out.placements.end()) continue;  // view not built (§6.7, §6.8)
        auto st = models.model("", A->id);
        if (!st || !*st) continue;
        const auto af = detail::resolveAnnotationFace(models.kernel(), **st, m.source);
        if (!af) {
            lost();
            continue;
        }
        auto it = specs.find(A->id);
        if (it == specs.end()) it = specs.emplace(A->id, assemblySpecContent(d, A->id, models)).first;
        const auto number = balloonNumber(*A, it->second, m.source);
        if (!number) {
            lost();
            continue;
        }
        const Point2 S = pl->offset + pl->scale * Point2(af->anchor.dot(t->r), af->anchor.dot(t->u));
        const Point2 diag(std::cos(PI / 4), std::sin(PI / 4));
        Point2 E = m.position ? Point2((*m.position)[0], (*m.position)[1]) : Point2(S + BALLOON_LEADER_MM * diag);
        if ((E - S).norm() < MARK_MIN_LEADER_MM) {
            const Point2 o = (E - S).norm() > 1e-9 ? Point2((E - S).normalized()) : diag;
            E = S + MARK_MIN_LEADER_MM * o;
        }
        const double sigma = E.x() >= S.x() ? 1.0 : -1.0;
        const double L = textW(*number, BALLOON_TEXT_H) + 2 * BALLOON_SHELF_GAP_MM;
        const std::string role = "view:" + m.view + ":balloon:" + m.id;
        // The dot on the part (GOST 2.316): a filled closed polyline.
        Primitive dot;
        dot.kind = PrimitiveKind::Polyline;
        dot.closed = true;
        dot.filled = true;
        dot.weight = Weight::Thin;
        dot.role = role;
        for (int i = 0; i < 16; ++i) {
            const double a = 2 * PI * i / 16;
            dot.points.push_back(S + BALLOON_DOT_R_MM * Point2(std::cos(a), std::sin(a)));
        }
        out.primitives.push_back(std::move(dot));
        out.primitives.push_back(line(S, E, role));
        out.primitives.push_back(line(E, E + Point2(sigma * L, 0), role));
        Primitive txt;
        txt.kind = PrimitiveKind::Text;
        txt.position = E + Point2(sigma * L / 2, BALLOON_TEXT_GAP_MM);
        txt.text = *number;
        txt.height = BALLOON_TEXT_H;
        txt.align = "center bottom";
        txt.role = role;
        out.primitives.push_back(std::move(txt));
        out.marks.push_back({m.id, E, {role}});
    }
}

void appendPartsLists(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                      SheetGraphics& out) {
    std::optional<SpecContent> content;
    for (const auto& m : sheet.marks) {
        if (m.kind != "partsList") continue;
        const std::string A = drawingModel(drawing);
        if (!assemblyOfThisDocument(d, A)) {
            out.errors.push_back({m.id, Error("DRAWING_MARK_SOURCE_LOST").with("source", A)});
            continue;
        }
        if (!content) content = assemblySpecContent(d, A, models);
        const SheetSize sz = partsListSize(*content);
        const Point2 P = m.position ? Point2((*m.position)[0], (*m.position)[1])
                                    : Point2(out.width - PARTS_LIST_RIGHT_MARGIN_MM - sz.width, PARTS_LIST_BOTTOM_MM + sz.height);
        const std::string role = "partsList:" + m.id;
        appendPartsListTable(*content, P, role, out);
        out.marks.push_back({m.id, P, {role}});
    }
}

}  // namespace construe::drawing
