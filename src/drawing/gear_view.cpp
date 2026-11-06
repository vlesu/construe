// Gear rims on drawings (spec/drawing.md §6.20): conventional representation by GOST 2.402 — spaces filled up to the
// tip circle, pitch lines; parameter table by GOST 2.403 in the top right corner of the sheet.
#include "drawing/standard.h"
#include "drawing/view_parts.h"
#include "gear/gear.h"
#include "pmi/pmi.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

namespace construe::drawing {

namespace detail {

namespace {

constexpr double ON_AXIS = 1e-6;  // model mm: the cutting plane passes through the gear axis (§6.20.2)

Point2 project(const Frame& f, const Point3& p) { return {p.dot(f.r), p.dot(f.u)}; }

// Union cache (§6.20.1): the same body and fill give the same shape, so the view projection cache hits.
struct FillCache {
    std::mutex mu;
    std::map<std::pair<const void*, const void*>, std::pair<std::pair<kernel::Shape, kernel::Shape>, kernel::Shape>> entries;
};
FillCache& fillCache() {
    static FillCache c;
    return c;
}

bool overlaps(const BBox3& a, const BBox3& b) { return !a.isEmpty() && !b.isEmpty() && a.intersects(b); }

// Angle in the gear plane of a direction w (counterclockwise about g.dir from g.ref).
double gearAngle(const regen::GearEntry& g, const Vec3& w) {
    const Vec3 y = g.dir.cross(g.ref);
    return std::atan2(w.dot(y), w.dot(g.ref));
}
bool inSector(const regen::GearEntry& g, double a) {
    if (!g.sector) return true;
    return geom2::ccwSweep(g.start, a) <= g.angle + 1e-9;
}

}  // namespace

std::vector<kernel::PlacedShape> gearConventionalBodies(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies,
                                                        const std::vector<regen::GearEntry>& gears) {
    std::vector<kernel::PlacedShape> out = bodies;
    for (const auto& g : gears) {
        if (g.fill.null()) continue;
        const BBox3 fb = k.bbox(g.fill);
        for (auto& b : out) {
            if (!overlaps(k.bbox(b.shape), fb)) continue;
            FillCache& c = fillCache();
            const auto key = std::make_pair(static_cast<const void*>(b.shape.data()), static_cast<const void*>(g.fill.data()));
            {
                std::lock_guard<std::mutex> lock(c.mu);
                if (auto it = c.entries.find(key); it != c.entries.end()) {
                    b.shape = it->second.second;
                    continue;
                }
            }
            auto u = k.boolean(kernel::BoolKind::Union, b.shape, g.fill);
            if (!u) continue;  // the view keeps the teeth (the projection still succeeds)
            std::lock_guard<std::mutex> lock(c.mu);
            if (c.entries.size() >= GEAR_FILL_CACHE) c.entries.clear();
            c.entries[key] = {{b.shape, g.fill}, u->shape};
            b.shape = u->shape;
        }
    }
    return out;
}

void appendGears(const std::vector<regen::GearEntry>& gears, const Frame& f, const ToSheet& toSheet, double scale,
                 const std::optional<Cut>& cut, const std::string& role, std::vector<Primitive>& out) {
    for (const auto& g : gears) {
        const Vec3 a = g.dir.normalized();
        const Point3& A = g.origin;
        // Removed by the section: the whole rim on the removed side (§6.11).
        if (cut) {
            const double g0 = (A - cut->origin).dot(cut->normal), g1 = (A + g.width * a - cut->origin).dot(cut->normal);
            if (std::abs(a.dot(cut->normal)) <= AXIS_PARALLEL_TOL ? g0 > g.da / 2 : (g0 > 0 && g1 > 0)) continue;
        }
        const double an = std::abs(a.dot(f.n));
        if (1 - an <= AXIS_PARALLEL_TOL) {  // end-on: the pitch circle (arc of the sector)
            Primitive p;
            p.center = toSheet(project(f, A));
            p.radius = scale * g.d / 2;
            p.lineType = LineType::DashDot;
            p.weight = Weight::Thin;
            p.role = role;
            if (!g.sector) {
                p.kind = PrimitiveKind::Circle;
            } else {
                const Vec3 y = a.cross(g.ref);
                auto viewAngle = [&](double t) {
                    const Vec3 w = std::cos(t) * g.ref + std::sin(t) * y;
                    return std::atan2(w.dot(f.u), w.dot(f.r));
                };
                double s = viewAngle(g.start), e = viewAngle(g.start + g.angle);
                if (a.dot(f.n) < 0) std::swap(s, e);  // seen from the other side: the sector runs clockwise in the view
                while (e <= s) e += 2 * PI;
                p.kind = PrimitiveKind::Arc;
                p.start = s, p.end = e;
            }
            out.push_back(std::move(p));
        } else if (an <= AXIS_PARALLEL_TOL) {  // from the side: pitch generatrices; in an axial section — root lines
            const bool axial = cut && std::abs(a.dot(cut->normal)) <= AXIS_PARALLEL_TOL && std::abs((A - cut->origin).dot(cut->normal)) <= ON_AXIS;
            if (g.internal && !axial) continue;  // inside the hole: hidden in a view
            const Point2 p0 = project(f, A), p1 = project(f, A + g.width * a);
            const Point2 ad = (p1 - p0).normalized();
            const Point2 perp(-ad.y(), ad.x());
            const double over = CENTERLINE_OVERSHOOT_MM / scale;
            for (double sgn : {1.0, -1.0}) {
                const Vec3 w = sgn * (perp.x() * f.r + perp.y() * f.u);
                if (!inSector(g, gearAngle(g, w))) continue;
                out.push_back(makeLine(toSheet(p0 - over * ad + sgn * g.d / 2 * perp), toSheet(p1 + over * ad + sgn * g.d / 2 * perp),
                                       LineType::DashDot, Weight::Thin, role));
                if (axial)
                    out.push_back(makeLine(toSheet(p0 + sgn * g.df / 2 * perp), toSheet(p1 + sgn * g.df / 2 * perp), LineType::Solid,
                                           Weight::Thick, role));
            }
        }
    }
}

}  // namespace detail

namespace {

using detail::makeLine;
using detail::makeText;

std::string num(double v) { return pmi::formatNumber(v, GEAR_TABLE_DECIMALS); }

// Text height fitted into a cell of width w (§6.20.3): GEAR_TABLE_TEXT or less, 0.7·h per character (as §6.13).
double fitHeight(const std::string& text, double w) {
    double chars = 0;
    for (unsigned char ch : text) chars += (ch & 0xC0) != 0x80;
    if (chars == 0) return GEAR_TABLE_TEXT;
    return std::min(GEAR_TABLE_TEXT, (w - 2 * GEAR_TABLE_PAD) / (0.7 * chars));
}

struct SheetGears {
    std::vector<std::pair<std::string, regen::GearEntry>> gears;  // (model ID, gear) in order
};

// Gears of the models whose first base view of the drawing is on this sheet (§6.20.3).
SheetGears sheetGears(const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models) {
    SheetGears out;
    std::set<std::string> seen;
    for (const auto& sh : drawing.sheets)
        for (const auto& v : sh.views) {
            if (v.kind != "base") continue;
            const std::string key = v.refFile + "#" + v.refElement;
            if (!seen.insert(key).second || &sh != &sheet) continue;
            auto st = models.model(v.refFile, v.refElement);
            if (!st || !*st) continue;
            for (const auto& g : (*st)->gears) out.gears.push_back({v.refElement, g});
        }
    return out;
}

}  // namespace

double gearTablesWidth(const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models) {
    return sheetGears(drawing, sheet, models).gears.empty() ? 0 : GEAR_TABLE_W;
}

void appendGearTables(const doc::Document&, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                      SheetGraphics& out) {
    const SheetGears sg = sheetGears(drawing, sheet, models);
    if (sg.gears.empty()) return;
    const double field = drawingStandard(drawing) == font::DrawingStandard::Iso ? ISO_FIELD : 5;
    const double xr = out.width - field, x0 = xr - GEAR_TABLE_W;
    const double xName = x0 + GEAR_TABLE_NAME_W, xSym = xName + GEAR_TABLE_SYMBOL_W;
    double top = out.height - field;
    for (const auto& [model, g] : sg.gears) {
        struct Row {
            std::string name, symbol, value;
            bool separator = false;  // a main line above: the reference data part (GOST 2.403)
        };
        std::vector<Row> rows = {{"Модуль", "m", num(g.module)}, {"Число зубьев", "z", std::to_string(g.teeth)}};
        if (g.sector) rows.push_back({"Число зубьев сектора", "", std::to_string(g.formed)});
        rows.push_back({"Нормальный исходный контур", "", gear::basicRack().designation});
        rows.push_back({"Коэффициент смещения", "x", num(g.shift)});
        rows.push_back({"Степень точности по ГОСТ 1643-81", "", g.accuracy});
        rows.push_back({"Делительный диаметр", "d", num(g.d), true});
        const std::string role = "gear:table:" + model + ":" + g.feature;
        const double h = GEAR_TABLE_ROW, bottom = top - h * double(rows.size());
        // Lines: outline and the reference-data separator — main; rows and columns — thin.
        out.primitives.push_back(makeLine({x0, top}, {xr, top}, LineType::Solid, Weight::Thick, role));
        out.primitives.push_back(makeLine({x0, bottom}, {xr, bottom}, LineType::Solid, Weight::Thick, role));
        out.primitives.push_back(makeLine({x0, bottom}, {x0, top}, LineType::Solid, Weight::Thick, role));
        out.primitives.push_back(makeLine({xr, bottom}, {xr, top}, LineType::Solid, Weight::Thick, role));
        out.primitives.push_back(makeLine({xName, bottom}, {xName, top}, LineType::Solid, Weight::Thin, role));
        out.primitives.push_back(makeLine({xSym, bottom}, {xSym, top}, LineType::Solid, Weight::Thin, role));
        for (size_t i = 0; i < rows.size(); ++i) {
            const double y = top - h * double(i);
            if (i > 0) out.primitives.push_back(makeLine({x0, y}, {xr, y}, LineType::Solid, rows[i].separator ? Weight::Thick : Weight::Thin, role));
            const double ym = y - h / 2;
            out.primitives.push_back(makeText({x0 + GEAR_TABLE_PAD, ym}, rows[i].name, fitHeight(rows[i].name, GEAR_TABLE_NAME_W),
                                              "left middle", role));
            if (!rows[i].symbol.empty())
                out.primitives.push_back(makeText({(xName + xSym) / 2, ym}, rows[i].symbol, fitHeight(rows[i].symbol, GEAR_TABLE_SYMBOL_W),
                                                  "center middle", role));
            if (!rows[i].value.empty())
                out.primitives.push_back(makeText({(xSym + xr) / 2, ym}, rows[i].value, fitHeight(rows[i].value, xr - xSym),
                                                  "center middle", role));
        }
        top = bottom;
    }
}

}  // namespace construe::drawing
