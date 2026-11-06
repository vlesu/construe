// Outer contour of a hidden line projection (spec/kernel-interface.md §3.17 "Outer contour", FX7-DRW-001): pieces of
// the outer boundary that the projection algorithm classified hidden or smooth are made visible.
#include "kernel/kernel.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>

namespace construe::kernel {

namespace {

// Points along a curve, no farther apart than step.
std::vector<Point2> samples(const ViewCurve& c, double step) {
    std::vector<Point2> out;
    auto segment = [&](const Point2& a, const Point2& b, bool first) {
        const int n = std::max(1, int(std::ceil((b - a).norm() / step)));
        for (int k = first ? 0 : 1; k <= n; ++k) out.push_back(a + (b - a) * (double(k) / n));
    };
    auto arc = [&](double a0, double sweep) {
        const int n = std::max(8, int(std::ceil(c.radius * sweep / step)));
        for (int k = 0; k <= n; ++k) {
            const double a = a0 + sweep * k / n;
            out.push_back(c.center + c.radius * Point2(std::cos(a), std::sin(a)));
        }
    };
    switch (c.kind) {
        case ViewCurve::Kind::Line: segment(c.p1, c.p2, true); break;
        case ViewCurve::Kind::Circle: arc(0, 2 * std::numbers::pi); break;
        case ViewCurve::Kind::Arc: arc(c.start, c.end - c.start); break;
        case ViewCurve::Kind::Polyline:
            for (size_t i = 0; i + 1 < c.points.size(); ++i) segment(c.points[i], c.points[i + 1], i == 0);
            if (c.points.size() == 1) out.push_back(c.points[0]);
            break;
    }
    return out;
}

}  // namespace

void promoteOuterContour(HlrProjection& p) {
    const std::vector<ViewCurve>* all[] = {&p.visible, &p.hidden, &p.visibleSmooth, &p.hiddenSmooth};
    // Extent of all curves (a coarse sampling is enough for the box).
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto* list : all)
        for (const auto& c : *list)
            for (const auto& q : samples(c, 1e300)) x0 = std::min(x0, q.x()), y0 = std::min(y0, q.y()), x1 = std::max(x1, q.x()), y1 = std::max(y1, q.y());
    if (!(x1 > x0) && !(y1 > y0)) return;
    const double px = std::max(x1 - x0, y1 - y0) / KERNEL_HLR_CONTOUR_RASTER;
    if (!(px > 0)) return;
    const int margin = 2;
    const int W = int(std::ceil((x1 - x0) / px)) + 1 + 2 * margin, H = int(std::ceil((y1 - y0) / px)) + 1 + 2 * margin;
    std::vector<unsigned char> any(size_t(W) * H, 0), visible(size_t(W) * H, 0), outside(size_t(W) * H, 0);
    auto cell = [&](const Point2& q) {
        return std::pair{std::clamp(int(std::floor((q.x() - x0) / px)) + margin, 0, W - 1),
                         std::clamp(int(std::floor((q.y() - y0) / px)) + margin, 0, H - 1)};
    };
    auto at = [&](int i, int j) { return size_t(j) * size_t(W) + size_t(i); };
    const double step = 0.25 * px;
    for (const auto* list : all)
        for (const auto& c : *list)
            for (const auto& q : samples(c, step)) {
                const auto [i, j] = cell(q);
                any[at(i, j)] = 1;
                if (list == &p.visible) visible[at(i, j)] = 1;
            }
    // The background: flooded from the border through pixels without curves (4-connectivity).
    std::deque<std::pair<int, int>> queue;
    auto seed = [&](int i, int j) {
        if (!any[at(i, j)] && !outside[at(i, j)]) outside[at(i, j)] = 1, queue.push_back({i, j});
    };
    for (int i = 0; i < W; ++i) seed(i, 0), seed(i, H - 1);
    for (int j = 0; j < H; ++j) seed(0, j), seed(W - 1, j);
    while (!queue.empty()) {
        const auto [i, j] = queue.front();
        queue.pop_front();
        if (i > 0) seed(i - 1, j);
        if (i + 1 < W) seed(i + 1, j);
        if (j > 0) seed(i, j - 1);
        if (j + 1 < H) seed(i, j + 1);
    }
    // A sample is uncovered contour: its pixel touches the background and no visible curve is within a pixel.
    auto uncovered = [&](const Point2& q) {
        const auto [i, j] = cell(q);
        bool touches = false;
        static constexpr int NEIGHBOURS[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& nb : NEIGHBOURS) {
            const int ni = i + nb[0], nj = j + nb[1];
            if (ni >= 0 && ni < W && nj >= 0 && nj < H && outside[at(ni, nj)]) touches = true;
        }
        if (!touches) return false;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di)
                if (i + di >= 0 && i + di < W && j + dj >= 0 && j + dj < H && visible[at(i + di, j + dj)]) return false;
        return true;
    };
    for (auto* list : {&p.hidden, &p.visibleSmooth, &p.hiddenSmooth}) {
        std::vector<ViewCurve> keep;
        for (auto& c : *list) {
            const auto qs = samples(c, step);
            const auto n = std::count_if(qs.begin(), qs.end(), uncovered);
            if (qs.size() >= 8 && double(n) >= KERNEL_HLR_CONTOUR_SHARE * double(qs.size())) p.visible.push_back(std::move(c));
            else keep.push_back(std::move(c));
        }
        *list = std::move(keep);
    }
}

}  // namespace construe::kernel
