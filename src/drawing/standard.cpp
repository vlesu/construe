// Drawing standard (spec/drawing.md §2.5, §3.2, §6.2; S3-DRW-008): projection method, drawing font, moving
// projected views on method change.
#include "drawing/standard.h"

#include "drawing/render.h"

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <utility>

namespace construe::drawing {

namespace {

std::mutex& prefMutex() {
    static std::mutex m;
    return m;
}
std::string& prefRef(font::DrawingStandard s) {
    static std::string gost, iso;
    return s == font::DrawingStandard::Iso ? iso : gost;
}

thread_local std::optional<std::string> scopedFont;  // §3.2: font for empty font within the build scope

std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

}  // namespace

font::DrawingStandard drawingStandard(const doc::Element& drawing) {
    return drawing.standard && *drawing.standard == "iso" ? font::DrawingStandard::Iso : font::DrawingStandard::Gost;
}

ProjectionMethod projectionMethod(const doc::Element& drawing) {
    if (drawingStandard(drawing) != font::DrawingStandard::Iso) return ProjectionMethod::First;
    return drawing.projection && *drawing.projection == "third" ? ProjectionMethod::Third : ProjectionMethod::First;
}

void setPreferredDrawingFont(font::DrawingStandard s, std::string name) {
    std::lock_guard lk(prefMutex());
    prefRef(s) = std::move(name);
}

std::string preferredDrawingFont(font::DrawingStandard s) {
    std::lock_guard lk(prefMutex());
    return prefRef(s);
}

std::string drawingFontFor(font::DrawingStandard s) {
    if (const std::string p = preferredDrawingFont(s); !p.empty()) {
        const std::string lp = lowerAscii(p);
        for (const auto& n : font::availableFonts())
            if (lowerAscii(n) == lp) return n;
    }
    std::string f = font::defaultDrawingFont(s);
    return f.empty() ? std::string(DRAWING_DEFAULT_FONT) : f;
}

std::string drawingFont(const doc::Element& drawing) { return drawingFontFor(drawingStandard(drawing)); }

DrawingFontScope::DrawingFontScope(std::string font) : prev_(scopedFont) { scopedFont = std::move(font); }

DrawingFontScope::~DrawingFontScope() { scopedFont = std::move(prev_); }

std::string scopedDrawingFont() { return scopedFont ? *scopedFont : std::string(DRAWING_DEFAULT_FONT); }

void applyDrawingFont(std::vector<Primitive>& prims, const std::string& font) {
    if (font.empty() || font == DRAWING_DEFAULT_FONT) return;
    for (auto& p : prims)
        if (p.kind == PrimitiveKind::Text && p.font.empty()) p.font = font;
}

void mirrorProjectedViews(doc::Element& drawing) {
    for (auto& sh : drawing.sheets) {
        const doc::Sheet old = sh;  // effective positions before the change (§6.2)
        std::map<std::string, Point2> done;
        std::function<Point2(const doc::DrawingView&, int)> newPos = [&](const doc::DrawingView& v, int depth) -> Point2 {
            if (const auto it = done.find(v.id); it != done.end()) return it->second;
            Point2 out(v.position[0], v.position[1]);
            if (v.kind == "projected" && depth <= int(old.views.size())) {
                const auto q = effectiveViewPosition(old, v.parent);
                const auto e = effectiveViewPosition(old, v.id);
                const int pi = old.viewIndex(v.parent);
                if (q && e && pi >= 0) {
                    const Point2 qn = newPos(old.views[size_t(pi)], depth + 1);
                    out = v.direction == "top" || v.direction == "bottom" ? Point2(qn.x(), qn.y() - (e->y() - q->y()))
                                                                          : Point2(qn.x() - (e->x() - q->x()), qn.y());
                }
            }
            done[v.id] = out;
            return out;
        };
        for (size_t i = 0; i < old.views.size(); ++i) {
            if (old.views[i].kind != "projected") continue;
            const Point2 p = newPos(old.views[i], 0);
            sh.views[i].position = {p.x(), p.y()};
        }
    }
}

ThreeViewsLayout threeViewsLayout(const BBox3& box, double k, ProjectionMethod method, const Point2& pos) {
    // Main view front: width — x, height — y; top view — height z; side view (left/right) — width z (§6.1, §6.2).
    const Vec3 ext = box.isEmpty() ? Vec3::Zero() : Vec3(box.max() - box.min());
    const double dy = k * (ext.y() + ext.z()) / 2 + THREE_VIEWS_GAP_MM;
    const double dx = k * (ext.x() + ext.z()) / 2 + THREE_VIEWS_GAP_MM;
    ThreeViewsLayout l;
    l.topDirection = "top";
    if (method == ProjectionMethod::First) {
        l.top = Point2(pos.x(), pos.y() - dy);   // first angle: top view — below the main one
        l.sideDirection = "left";                  // left view — to the right of the main one
    } else {
        l.top = Point2(pos.x(), pos.y() + dy);   // third angle: top view — above the main one
        l.sideDirection = "right";                 // right view — to the right of the main one
    }
    l.side = Point2(pos.x() + dx, pos.y());
    return l;
}

bool Rect2::contains(const Rect2& o) const {
    constexpr double eps = 1e-9;
    return o.min.x() >= min.x() - eps && o.min.y() >= min.y() - eps && o.max.x() <= max.x() + eps && o.max.y() <= max.y() + eps;
}

bool Rect2::overlaps(const Rect2& o) const {
    constexpr double eps = 1e-9;
    return o.min.x() < max.x() - eps && o.max.x() > min.x() + eps && o.min.y() < max.y() - eps && o.max.y() > min.y() + eps;
}

SheetField sheetField(const doc::Element& drawing, const doc::Sheet& sheet) {
    SheetField f;
    const auto size = sheetSize(drawing, sheet);
    if (!size) return f;
    const double W = size->width, H = size->height, M = THREE_VIEWS_MARGIN_MM;
    const Point2 m(M, M);
    auto block = [&](Point2 a, Point2 b) { f.blocked.push_back(Rect2{a - m, b + m}); };
    if (drawingStandard(drawing) == font::DrawingStandard::Iso) {  // §5а.1, §5а.2
        f.frame = Rect2{Point2(ISO_FIELD_LEFT, ISO_FIELD) + m, Point2(W - ISO_FIELD, H - ISO_FIELD) - m};
        block(Point2(W - ISO_FIELD - ISO_BLOCK_W, ISO_FIELD), Point2(W - ISO_FIELD, ISO_FIELD + ISO_BLOCK_H));
        return f;
    }
    // GOST (§5.2, §5.3, §5.5): frame 20 / 5 mm, title block 185 wide (form 1 — 55 high on the first sheet, form 2a — 15),
    // field 26 in the top left corner (top right along the right side on portrait sheets larger than A4).
    f.frame = Rect2{Point2(20, 5) + m, Point2(W - 5, H - 5) - m};
    const bool first = drawing.sheets.empty() || drawing.sheets.front().id == sheet.id;
    block(Point2(W - 5 - 185, 5), Point2(W - 5, 5 + (first ? 55 : 15)));
    if (W < H && !(W == 210 && H == 297)) block(Point2(W - 19, H - 75), Point2(W - 5, H - 5));
    else block(Point2(20, H - 19), Point2(90, H - 5));
    return f;
}

namespace {

// Nearest main view position to want at which the three views of scale k lie in the field (§6.2б).
std::optional<Point2> placeThreeViews(const Vec3& ext, double k, ProjectionMethod method, const Point2& want, const SheetField& field) {
    const double a = k * ext.x(), b = k * ext.y(), c = k * ext.z();
    const double dy = (b + c) / 2 + THREE_VIEWS_GAP_MM, dx = (a + c) / 2 + THREE_VIEWS_GAP_MM;
    // View rectangles relative to the main view position: center and half size (§6.2а).
    const std::array<std::pair<Point2, Point2>, 3> views{{{Point2(0, 0), Point2(a / 2, b / 2)},
                                                          {Point2(0, method == ProjectionMethod::First ? -dy : dy), Point2(a / 2, c / 2)},
                                                          {Point2(dx, 0), Point2(c / 2, b / 2)}}};
    // Positions keeping every view inside the frame.
    Point2 lo(-1e300, -1e300), hi(1e300, 1e300);
    for (const auto& [ctr, half] : views) {
        lo = lo.cwiseMax(Point2(field.frame.min - (ctr - half)));
        hi = hi.cwiseMin(Point2(field.frame.max - (ctr + half)));
    }
    if (lo.x() > hi.x() + 1e-9 || lo.y() > hi.y() + 1e-9) return std::nullopt;
    auto valid = [&](const Point2& p) {
        for (const auto& [ctr, half] : views) {
            const Rect2 r{p + ctr - half, p + ctr + half};
            if (!field.frame.contains(r)) return false;
            for (const auto& bl : field.blocked)
                if (bl.overlaps(r)) return false;
        }
        return true;
    };
    // The nearest free point has each coordinate either from want or on an edge of the allowed box or of a region
    // forbidden by a blocked rectangle — try all such combinations.
    std::vector<double> xs{want.x(), lo.x(), hi.x()}, ys{want.y(), lo.y(), hi.y()};
    for (const auto& [ctr, half] : views)
        for (const auto& bl : field.blocked) {
            xs.push_back(bl.min.x() - ctr.x() - half.x());
            xs.push_back(bl.max.x() - ctr.x() + half.x());
            ys.push_back(bl.min.y() - ctr.y() - half.y());
            ys.push_back(bl.max.y() - ctr.y() + half.y());
        }
    std::optional<Point2> best;
    double bestD = 0;
    for (double x : xs)
        for (double y : ys) {
            const Point2 p(std::clamp(x, lo.x(), std::max(lo.x(), hi.x())), std::clamp(y, lo.y(), std::max(lo.y(), hi.y())));
            if (!valid(p)) continue;
            const double d = (p - want).squaredNorm();
            if (!best || d < bestD - 1e-12) best = p, bestD = d;
        }
    return best;
}

}  // namespace

ThreeViewsFit fitThreeViews(const BBox3& box, const std::string& scale, bool autoScale, ProjectionMethod method,
                            const Point2& want, const SheetField& field) {
    const Vec3 ext = box.isEmpty() ? Vec3::Zero() : Vec3(box.max() - box.min());
    const double k0 = doc::scaleValue(scale).value_or(1.0);
    ThreeViewsFit r{scale, want, false};
    if (auto p = placeThreeViews(ext, k0, method, want, field)) return {scale, *p, true};
    if (!autoScale) return r;
    // The standard series (§2.2) by decreasing value.
    std::vector<std::pair<double, std::string>> series;
    for (const auto& s : standardScales()) series.emplace_back(doc::scaleValue(s).value_or(1.0), s);
    std::sort(series.begin(), series.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [k, s] : series) {
        if (k >= k0) continue;
        r.scale = s;
        if (auto p = placeThreeViews(ext, k, method, want, field)) return {s, *p, true};
    }
    return r;
}

}  // namespace construe::drawing
