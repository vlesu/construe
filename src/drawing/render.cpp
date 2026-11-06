// Rendering of sheet primitives: dashes, texts as outlines, arcs as Bezier curves (spec/drawing.md §3, §3.1).
#include "drawing/render.h"

#include "drawing/standard.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace construe::drawing {

namespace {

// Counterclockwise arc angle from a0 to a1, reduced to (0, 2π].
double sweepOf(double a0, double a1) { return geom2::ccwSweepPositive(a0, a1); }

// Greatest X of an outline curve (§3.1 "Width").
double maxX(const font::Curve2& c) {
    double m = std::max(c.a.x(), c.b.x());
    if (c.kind == font::Curve2::Kind::Arc) {
        const double r = (c.a - c.center).norm();
        double a0 = std::atan2(c.a.y() - c.center.y(), c.a.x() - c.center.x());
        if (a0 < 0) a0 += TWO_PI;
        const double s = sweepOf(a0, std::atan2(c.b.y() - c.center.y(), c.b.x() - c.center.x()));
        if (a0 + s >= TWO_PI) m = std::max(m, c.center.x() + r);
    }
    return m;
}

struct Align {
    int h = 0;  // 0 — left, 1 — center, 2 — right
    int v = 0;  // 0 — bottom, 1 — middle, 2 — top
};
Align parseAlign(const std::string& s) {
    Align a;
    std::istringstream in(s);
    std::string w;
    while (in >> w) {
        if (w == "left") a.h = 0;
        else if (w == "center") a.h = 1;
        else if (w == "right") a.h = 2;
        else if (w == "bottom") a.v = 0;
        else if (w == "middle") a.v = 1;
        else if (w == "top") a.v = 2;
    }
    return a;
}

Result<font::Layout> layoutAtOrigin(const std::string& text, double height, const std::string& fontName,
                                    const std::vector<font::StyleSpan>& styles = {}) {
    auto f = font::loadFont(fontName.empty() ? scopedDrawingFont() : fontName);  // §3.2
    if (!f) return f.error();
    return font::layoutText(**f, text, height, Point2(0, 0), 0, styles);
}

double widthOf(const font::Layout& l) {
    bool any = false;
    double w = 0;
    for (const auto& c : l.contours)
        for (const auto& cv : c) {
            const double x = maxX(cv);
            w = any ? std::max(w, x) : x;
            any = true;
        }
    return any ? w : 0;
}

}  // namespace

std::vector<double> dashPattern(LineType t) {
    switch (t) {
        case LineType::Dashed: return {DASH_STROKE_MM, DASH_GAP_MM};
        case LineType::DashDot: return {DASHDOT_STROKE_MM, DASHDOT_GAP_MM / 2, DOT_MM, DASHDOT_GAP_MM / 2};
        case LineType::DashDotDot:
            return {DASHDOTDOT_STROKE_MM, DASHDOTDOT_GAP_MM / 3, DOT_MM, DASHDOTDOT_GAP_MM / 3, DOT_MM, DASHDOTDOT_GAP_MM / 3};
        default: return {};
    }
}

Result<double> textWidth(const std::string& text, double height, const std::string& fontName, const std::vector<font::StyleSpan>& styles) {
    auto l = layoutAtOrigin(text, height, fontName, styles);
    if (!l) return l.error();
    return widthOf(*l);
}

Result<TextOutline> textOutline(const Primitive& p) {
    auto l = layoutAtOrigin(p.text, p.height, p.font, p.styles);
    if (!l) return l.error();
    const double w = widthOf(*l);
    const Align al = parseAlign(p.align);
    const Point2 shift(al.h == 0 ? 0 : al.h == 1 ? -w / 2 : -w, al.v == 0 ? 0 : al.v == 1 ? -p.height / 2 : -p.height);
    const double cs = std::cos(p.angle), sn = std::sin(p.angle);
    auto place = [&](const Point2& q) {
        const Point2 s = q + shift;
        return Point2(p.position.x() + cs * s.x() - sn * s.y(), p.position.y() + sn * s.x() + cs * s.y());
    };
    TextOutline out;
    out.warnings = std::move(l->warnings);
    out.contours.reserve(l->contours.size());
    for (const auto& c : l->contours) {
        std::vector<font::Curve2> pc;
        pc.reserve(c.size());
        for (const auto& cv : c) {
            font::Curve2 n = cv;
            n.a = place(cv.a);
            n.b = place(cv.b);
            n.center = place(cv.center);
            pc.push_back(n);
        }
        out.contours.push_back(std::move(pc));
    }
    return out;
}

std::vector<std::array<Point2, 4>> arcBeziers(const Point2& c, double r, double a0, double a1) {
    const double sweep = sweepOf(a0, a1);
    const int n = std::max(1, int(std::ceil(sweep / (std::numbers::pi / 2) - 1e-12)));
    const double phi = sweep / n;
    const double k = 4.0 / 3.0 * std::tan(phi / 4);
    std::vector<std::array<Point2, 4>> out;
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const double al = a0 + i * phi, be = al + phi;
        const Point2 p0 = c + r * Point2(std::cos(al), std::sin(al));
        const Point2 p3 = c + r * Point2(std::cos(be), std::sin(be));
        const Point2 p1 = p0 + k * r * Point2(-std::sin(al), std::cos(al));
        const Point2 p2 = p3 - k * r * Point2(-std::sin(be), std::cos(be));
        out.push_back({p0, p1, p2, p3});
    }
    return out;
}

std::vector<std::array<Point2, 4>> arcBeziers(const font::Curve2& arc) {
    const double r = (arc.a - arc.center).norm();
    const double a0 = std::atan2(arc.a.y() - arc.center.y(), arc.a.x() - arc.center.x());
    const double a1 = std::atan2(arc.b.y() - arc.center.y(), arc.b.x() - arc.center.x());
    auto out = arcBeziers(arc.center, r, a0, a1);
    if (!out.empty()) {
        out.front()[0] = arc.a;
        out.back()[3] = arc.b;
    }
    return out;
}

std::vector<OrientedCurve> orientContour(const std::vector<font::Curve2>& contour) {
    std::vector<OrientedCurve> out;
    if (contour.empty()) return out;
    // Traversal start: for the first curve, the endpoint shared with the second one is its traversal end.
    Point2 cur = contour.front().a;
    if (contour.size() > 1) {
        const auto& c0 = contour[0];
        const auto& c1 = contour[1];
        auto dist = [&](const Point2& p) { return std::min((p - c1.a).norm(), (p - c1.b).norm()); };
        cur = dist(c0.a) < dist(c0.b) ? c0.b : c0.a;
    }
    for (const auto& c : contour) {
        OrientedCurve oc;
        oc.arc = c.kind == font::Curve2::Kind::Arc;
        oc.center = c.center;
        const bool forward = (c.a - cur).norm() <= (c.b - cur).norm();
        oc.from = forward ? c.a : c.b;
        oc.to = forward ? c.b : c.a;
        oc.ccw = forward;
        out.push_back(oc);
        cur = oc.to;
    }
    return out;
}

std::vector<std::array<Point2, 4>> arcBeziers(const OrientedCurve& arc) {
    font::Curve2 c;
    c.kind = font::Curve2::Kind::Arc;
    c.center = arc.center;
    c.a = arc.ccw ? arc.from : arc.to;
    c.b = arc.ccw ? arc.to : arc.from;
    auto bz = arcBeziers(c);
    if (arc.ccw) return bz;
    std::reverse(bz.begin(), bz.end());  // the same arc traversed backwards
    for (auto& b : bz) std::swap(b[0], b[3]), std::swap(b[1], b[2]);
    return bz;
}

}  // namespace construe::drawing
