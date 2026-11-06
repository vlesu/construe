// ISO 5457 frame and ISO 7200 title block (spec/drawing.md §5а, S3-DRW-008).
#include "drawing/render.h"
#include "drawing/standard.h"
#include "drawing/title_block.h"

#include <utility>

namespace construe::drawing {

namespace {

struct LineRow {
    double u0, v0, u1, v1;
    Weight w;
};
constexpr Weight K = Weight::Thick, T = Weight::Thin;

// Title block lines (§5а.2, table).
constexpr LineRow ISO_LINES[] = {
    {0, 0, 0, 36, K},    {0, 36, 180, 36, K}, {0, 27, 180, 27, T}, {0, 9, 180, 9, T},   {45, 18, 180, 18, T},
    {45, 0, 45, 36, T},  {90, 0, 90, 36, T},  {135, 27, 135, 36, T}, {20, 0, 20, 9, T}, {72, 0, 72, 9, T},
    {155, 9, 155, 18, T}, {130, 0, 130, 9, T}, {150, 0, 150, 9, T},
};

// Fields (§5а.4, table): cell, caption, value key, value height and fitting.
struct Cell {
    double u0, u1, v0, v1;
    const char* label;
    const char* key;
    double h, fit;
};
constexpr Cell ISO_CELLS[] = {
    {0, 45, 27, 36, "Responsible dept.", "department", 3.5, 43},
    {45, 90, 27, 36, "Technical reference", "technicalReference", 3.5, 43},
    {90, 135, 27, 36, "Document type", "documentType", 3.5, 43},
    {135, 180, 27, 36, "Document status", "documentStatus", 3.5, 43},
    {0, 45, 9, 27, "Legal owner", "organization", 5, 43},
    {45, 90, 18, 27, "Created by", "developer", 3.5, 43},
    {45, 90, 9, 18, "Approved by", "approver", 3.5, 43},
    {90, 180, 18, 27, "Title", "name", 5, 88},
    {90, 155, 9, 18, "Identification number", "designation", 5, 63},
    {155, 180, 9, 18, "Rev.", "revision", 3.5, 23},
    {0, 20, 0, 9, "Scale", "scale", 3.5, 18},
    {45, 72, 0, 9, "Material", "material", 3.5, 25},
    {72, 90, 0, 9, "Mass, kg", "mass", 3.5, 16},
    {90, 130, 0, 9, "Date of issue", "issueDate", 3.5, 38},
    {130, 150, 0, 9, "Lang.", "language", 3.5, 18},
    {150, 180, 0, 9, "Sheet", "sheet", 3.5, 28},
};

Primitive line(const Point2& a, const Point2& b, Weight w, const char* role, LineType lt = LineType::Solid) {
    Primitive p;
    p.kind = PrimitiveKind::Line;
    p.points = {a, b};
    p.weight = w;
    p.lineType = lt;
    p.role = role;
    return p;
}

// Field text (§5а.4, §5.7): an empty one is not added; with fitting the height is reduced to the fit width (drawing font, §3.2).
void addText(SheetGraphics& g, const Point2& pos, const std::string& text, double h, const char* align, double fit) {
    if (text.empty()) return;
    if (fit > 0)
        if (auto w = textWidth(text, h); w && *w > fit) h = h * fit / *w;
    Primitive p;
    p.kind = PrimitiveKind::Text;
    p.position = pos;
    p.text = text;
    p.height = h;
    p.align = align;
    p.role = "titleBlock";
    g.primitives.push_back(std::move(p));
}

// ISO 5456-2 projection method symbol (§5а.3); o — lower-left corner of the title block.
void appendProjectionSymbol(const Point2& o, ProjectionMethod m, SheetGraphics& g) {
    const double h = PROJECTION_SYMBOL_H;
    const Point2 c = o + Point2(32.5, 4.5);
    const double x0 = c.x() - 2.5 * h;
    const double t0 = m == ProjectionMethod::First ? x0 : x0 + 3 * h;  // narrow end of the trapezoid
    const Point2 cc(m == ProjectionMethod::First ? x0 + 4 * h : x0 + h, c.y());
    Primitive trap;
    trap.kind = PrimitiveKind::Polyline;
    trap.points = {Point2(t0, c.y() - h / 2), Point2(t0 + 2 * h, c.y() - h), Point2(t0 + 2 * h, c.y() + h), Point2(t0, c.y() + h / 2)};
    trap.closed = true;
    trap.role = "titleBlock";
    g.primitives.push_back(std::move(trap));
    for (double r : {h, h / 2}) {
        Primitive ci;
        ci.kind = PrimitiveKind::Circle;
        ci.center = cc;
        ci.radius = r;
        ci.role = "titleBlock";
        g.primitives.push_back(std::move(ci));
    }
    g.primitives.push_back(line(Point2(x0 - 1, c.y()), Point2(x0 + 5 * h + 1, c.y()), T, "titleBlock", LineType::DashDot));
    g.primitives.push_back(line(Point2(cc.x(), c.y() - h - 1), Point2(cc.x(), c.y() + h + 1), T, "titleBlock", LineType::DashDot));
}

}  // namespace

const std::vector<std::string>& isoTitleBlockKeys() {
    static const std::vector<std::string> keys = {"department", "technicalReference", "documentType", "documentStatus",
                                                  "revision",   "issueDate",          "language"};
    return keys;
}

void appendIsoTitleFrame(double W, double H, ProjectionMethod method, const std::map<std::string, std::string>& values,
                         SheetGraphics& g) {
    // 1. Frame and centering marks (§5а.1).
    {
        Primitive f;
        f.kind = PrimitiveKind::Polyline;
        f.points = {Point2(ISO_FIELD_LEFT, ISO_FIELD), Point2(W - ISO_FIELD, ISO_FIELD), Point2(W - ISO_FIELD, H - ISO_FIELD),
                    Point2(ISO_FIELD_LEFT, H - ISO_FIELD)};
        f.closed = true;
        f.weight = K;
        f.role = "frame";
        g.primitives.push_back(std::move(f));
        const double over = ISO_CENTRING_OVERSHOOT_MM;
        g.primitives.push_back(line(Point2(0, H / 2), Point2(ISO_FIELD_LEFT + over, H / 2), K, "frame"));
        g.primitives.push_back(line(Point2(W, H / 2), Point2(W - ISO_FIELD - over, H / 2), K, "frame"));
        g.primitives.push_back(line(Point2(W / 2, H), Point2(W / 2, H - ISO_FIELD - over), K, "frame"));
        const double y1 = W / 2 < W - ISO_FIELD - ISO_BLOCK_W ? ISO_FIELD + over : ISO_FIELD;
        g.primitives.push_back(line(Point2(W / 2, 0), Point2(W / 2, y1), K, "frame"));
    }
    // 2. Title block lines (§5а.2).
    const Point2 o(W - ISO_FIELD - ISO_BLOCK_W, ISO_FIELD);
    for (const auto& l : ISO_LINES) g.primitives.push_back(line(o + Point2(l.u0, l.v0), o + Point2(l.u1, l.v1), l.w, "titleBlock"));
    // 3. Projection method symbol (§5а.3).
    appendProjectionSymbol(o, method, g);
    // 4. Field captions and values (§5а.4); date templates — today's date (drawing.md §5.4а, FX14-DRW-008).
    const CalendarDate now = today();
    for (const auto& c : ISO_CELLS) {
        addText(g, o + Point2(c.u0 + 1, c.v1 - 1), c.label, ISO_LABEL_H, "left top", 0);
        const auto it = values.find(c.key);
        if (it != values.end())
            addText(g, o + Point2((c.u0 + c.u1) / 2, (c.v0 + c.v1 - 3) / 2),
                    hasDateTemplate(it->second) ? expandDateTemplates(it->second, now) : it->second, c.h, "center middle", c.fit);
    }
}

}  // namespace construe::drawing
