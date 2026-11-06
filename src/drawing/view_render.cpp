// Shaded ("render") display of a view (spec/drawing.md §6.22, FX7-DRW-007, FX8-DRW-003, FX8-DRW-006): a raster image
// of the body tessellation drawn with a depth buffer, part colors and the 3D view's headlight shading.
#include "drawing/view_parts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace construe::drawing::detail {

namespace {

// "#RRGGBB" → channels; anything else — RENDER_DEFAULT_COLOR.
std::array<double, 3> parseColor(const std::string& c) {
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    const std::string& s = c.size() == 7 && c[0] == '#' ? c : std::string(RENDER_DEFAULT_COLOR);
    std::array<double, 3> out{};
    for (int i = 0; i < 3; ++i) {
        const int h = hex(s[size_t(1 + 2 * i)]), l = hex(s[size_t(2 + 2 * i)]);
        if (h < 0 || l < 0) return parseColor(RENDER_DEFAULT_COLOR);
        out[size_t(i)] = double(16 * h + l);
    }
    return out;
}

}  // namespace

std::optional<ShadedImage> rasterizeShaded(const std::vector<ShadedTriangle>& tris, double dpi) {
    Eigen::AlignedBox2d box;
    for (const auto& t : tris)
        for (const auto& p : t.p) box.extend(p);
    if (box.isEmpty()) return std::nullopt;
    // Pixel grid: one pixel of margin around the box; the resolution drops if a side exceeds RENDER_MAX_PX.
    double ppm = dpi / 25.4;
    const Point2 size = box.max() - box.min();
    const double side = std::max(size.x(), size.y()) * ppm + 2;
    if (side > RENDER_MAX_PX) ppm *= (RENDER_MAX_PX - 2) / (side - 2);
    const Point2 origin = box.min() - Point2(1 / ppm, 1 / ppm);  // lower-left corner of the image
    const int W = int(std::ceil(size.x() * ppm)) + 2, H = int(std::ceil(size.y() * ppm)) + 2;
    const int S = RENDER_SUPERSAMPLE, SW = W * S, SH = H * S;
    const double sppm = ppm * S;  // samples per mm
    const double top = origin.y() + H / ppm;
    std::vector<float> depth(size_t(SW) * size_t(SH), -std::numeric_limits<float>::infinity());
    std::vector<std::array<uint8_t, 3>> color(size_t(SW) * size_t(SH));
    std::vector<uint8_t> covered(size_t(SW) * size_t(SH), 0);
    for (const auto& t : tris) {
        // Sample space: x right, y down; sample (i, j) has its center at (i + 0.5, j + 0.5).
        Point2 q[3];
        for (int v = 0; v < 3; ++v) q[v] = Point2((t.p[v].x() - origin.x()) * sppm, (top - t.p[v].y()) * sppm);
        const double area = (q[1] - q[0]).x() * (q[2] - q[0]).y() - (q[1] - q[0]).y() * (q[2] - q[0]).x();
        if (!(std::abs(area) > 0)) continue;  // edge-on triangle
        const int i0 = std::max(0, int(std::floor(std::min({q[0].x(), q[1].x(), q[2].x()}) - 0.5)));
        const int i1 = std::min(SW - 1, int(std::ceil(std::max({q[0].x(), q[1].x(), q[2].x()}) - 0.5)));
        const int j0 = std::max(0, int(std::floor(std::min({q[0].y(), q[1].y(), q[2].y()}) - 0.5)));
        const int j1 = std::min(SH - 1, int(std::ceil(std::max({q[0].y(), q[1].y(), q[2].y()}) - 0.5)));
        const double qx[3] = {q[0].x(), q[1].x(), q[2].x()}, qy[3] = {q[0].y(), q[1].y(), q[2].y()};
        const Vec3 n0 = t.normal[0], n1 = t.normal[1], n2 = t.normal[2];
        for (int j = j0; j <= j1; ++j) {
            const double cy = j + 0.5;
            // Weights are linear in x along the row: w_v = A_v·x + B_v — only the span where all are ≥ 0 is visited
            // (one sample of margin; the exact test below decides).
            double A[3], B[3], lo = i0, hi = i1 + 1.0;
            for (int v = 0; v < 3; ++v) {
                const int a = (v + 1) % 3, b = (v + 2) % 3;
                A[v] = -(qy[b] - qy[a]) / area;
                B[v] = ((qx[b] - qx[a]) * (cy - qy[a]) + (qy[b] - qy[a]) * qx[a]) / area;
                if (A[v] > 0) lo = std::max(lo, -B[v] / A[v]);
                else if (A[v] < 0) hi = std::min(hi, -B[v] / A[v]);
                else if (B[v] < 0) lo = hi + 1;
            }
            if (lo > hi + 1) continue;
            const int ia = std::max(i0, int(std::floor(lo - 0.5)) - 1), ib = std::min(i1, int(std::ceil(hi - 0.5)) + 1);
            for (int i = ia; i <= ib; ++i) {
                const double cx = i + 0.5;
                // Barycentric weights; a sample on an edge belongs to both triangles (the depth test decides).
                double w[3];
                for (int v = 0; v < 3; ++v) {
                    const int a = (v + 1) % 3, b = (v + 2) % 3;
                    w[v] = ((qx[b] - qx[a]) * (cy - qy[a]) - (qy[b] - qy[a]) * (cx - qx[a])) / area;
                }
                if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
                const double d = w[0] * t.depth[0] + w[1] * t.depth[1] + w[2] * t.depth[2];
                const size_t k = size_t(j) * size_t(SW) + size_t(i);
                if (!(float(d) > depth[k])) continue;  // nearer wins; an equal depth keeps the earlier triangle
                depth[k] = float(d);
                const double nx = w[0] * n0.x() + w[1] * n1.x() + w[2] * n2.x(), ny = w[0] * n0.y() + w[1] * n1.y() + w[2] * n2.y(),
                             nz = w[0] * n0.z() + w[1] * n1.z() + w[2] * n2.z();
                const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
                const double lit = len > 0 ? std::min(1.0, std::abs(nz) / len) : 1.0;
                const double I = RENDER_AMBIENT + (1 - RENDER_AMBIENT) * lit;
                for (int ch = 0; ch < 3; ++ch) color[k][size_t(ch)] = uint8_t(std::lround(std::clamp(t.color[size_t(ch)] * I, 0.0, 255.0)));
                covered[k] = 1;
            }
        }
    }
    // Box filter of S × S samples: alpha — the covered share, color — the mean of the covered samples.
    ShadedImage out;
    out.lowerLeft = origin;
    out.upperRight = Point2(origin.x() + W / ppm, top);
    auto img = std::make_shared<RasterImage>();
    img->width = W;
    img->height = H;
    img->rgba.assign(size_t(W) * size_t(H) * 4, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int count = 0;
            int sum[3] = {0, 0, 0};
            for (int sy = 0; sy < S; ++sy)
                for (int sx = 0; sx < S; ++sx) {
                    const size_t k = size_t(y * S + sy) * size_t(SW) + size_t(x * S + sx);
                    if (!covered[k]) continue;
                    ++count;
                    for (int ch = 0; ch < 3; ++ch) sum[ch] += color[k][size_t(ch)];
                }
            uint8_t* px = &img->rgba[(size_t(y) * size_t(W) + size_t(x)) * 4];
            if (count == 0) continue;
            for (int ch = 0; ch < 3; ++ch) px[ch] = uint8_t((2 * sum[ch] + count) / (2 * count));  // rounded mean
            px[3] = uint8_t((2 * 255 * count + S * S) / (2 * S * S));
        }
    out.image = std::move(img);
    return out;
}

namespace {

// Cache of shaded images (§6.22): the image depends on the bodies, their placements and colors, the view frame and the
// scale only — it is computed in view coordinates scaled by k and moved to the view position afterwards, so redrawing the
// sheet, moving the view and the repeated sheet builds of a regeneration reuse it.
struct ShadeKey {
    std::vector<std::pair<std::weak_ptr<const kernel::ShapeData>, const kernel::ShapeData*>> shapes;
    std::vector<kernel::Motion> motions;
    std::vector<std::string> colors;
    Vec3 n, u;
    double k = 0;
    bool same(const std::vector<kernel::PlacedShape>& b, const std::vector<std::string>& c, const Frame& f, double scale) const {
        if (b.size() != shapes.size() || c != colors || n != f.n || u != f.u || k != scale) return false;
        for (size_t i = 0; i < b.size(); ++i) {
            if (shapes[i].second != b[i].shape.data() || shapes[i].first.lock().get() != b[i].shape.data()) return false;
            if (motions[i].r != b[i].motion.r || motions[i].t != b[i].motion.t) return false;
        }
        return true;
    }
};
struct ShadeCache {
    std::mutex mu;
    std::vector<std::pair<ShadeKey, std::optional<ShadedImage>>> items;  // newest last
};
ShadeCache& shadeCache() {
    static ShadeCache c;
    return c;
}

std::optional<ShadedImage> shadedImage(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies,
                                       const std::vector<std::string>& colors, const Frame& f, double scale) {
    auto& cache = shadeCache();
    {
        std::lock_guard lk(cache.mu);
        for (const auto& [key, img] : cache.items)
            if (key.same(bodies, colors, f, scale)) return img;
    }
    std::vector<ShadedTriangle> tris;
    kernel::TessParams tp;
    tp.chordTol = std::clamp(RENDER_CHORD_MM / scale, kernel::KERNEL_MIN_CHORD, kernel::KERNEL_MAX_CHORD);
    tp.angleTol = RENDER_ANGLE_TOL;
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        const auto& b = bodies[bi];
        auto t = k.tessellate(b.shape, tp);
        if (!t) continue;
        const auto c = parseColor(bi < colors.size() ? colors[bi] : std::string());
        Eigen::Matrix3d R;
        const auto& m = b.motion.r;
        R << m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8];
        for (const auto& fm : t->faces) {
            auto P = [&](uint32_t i) {
                return Vec3(R * Vec3(fm.positions[3 * i], fm.positions[3 * i + 1], fm.positions[3 * i + 2]) + b.motion.t);
            };
            auto N = [&](uint32_t i) {
                if (fm.normals.size() < 3 * size_t(i) + 3) return Vec3(0, 0, 0);
                return Vec3(R * Vec3(fm.normals[3 * i], fm.normals[3 * i + 1], fm.normals[3 * i + 2]));
            };
            for (size_t i = 0; i + 2 < fm.triangles.size(); i += 3) {
                ShadedTriangle st;
                for (int v = 0; v < 3; ++v) {
                    const uint32_t idx = fm.triangles[i + size_t(v)];
                    const Vec3 X = P(idx);
                    st.p[v] = scale * Point2(X.dot(f.r), X.dot(f.u));  // view coordinates × k; moved to the sheet later
                    st.depth[v] = X.dot(f.n);
                    const Vec3 n = N(idx);
                    st.normal[v] = Vec3(n.dot(f.r), n.dot(f.u), n.dot(f.n));  // view frame: z — toward the viewer
                }
                const Vec3 A = P(fm.triangles[i]), B = P(fm.triangles[i + 1]), C = P(fm.triangles[i + 2]);
                const Vec3 fn = (B - A).cross(C - A);
                if (!(fn.norm() > 0)) continue;
                for (int v = 0; v < 3; ++v)  // no vertex normal — the flat normal of the triangle
                    if (!(st.normal[v].norm() > 0)) st.normal[v] = Vec3(fn.dot(f.r), fn.dot(f.u), fn.dot(f.n));
                st.color = c;
                tris.push_back(st);
            }
        }
    }
    auto img = rasterizeShaded(tris, RENDER_DPI);
    ShadeKey key;
    for (const auto& b : bodies) {
        key.shapes.push_back({b.shape.weak(), b.shape.data()});
        key.motions.push_back(b.motion);
    }
    key.colors = colors;
    key.n = f.n;
    key.u = f.u;
    key.k = scale;
    std::lock_guard lk(cache.mu);
    std::erase_if(cache.items, [](const auto& it) {
        return std::any_of(it.first.shapes.begin(), it.first.shapes.end(), [](const auto& w) { return w.first.expired(); });
    });
    if (cache.items.size() >= RENDER_CACHE) cache.items.erase(cache.items.begin());
    cache.items.push_back({std::move(key), img});
    return img;
}

}  // namespace

void appendShading(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies, const std::vector<std::string>& colors,
                   const Frame& f, const ToSheet& toSheet, double scale, const std::string& role, std::vector<Primitive>& out) {
    auto img = shadedImage(k, bodies, colors, f, scale);
    if (!img) return;
    const Point2 offset = toSheet(Point2(0, 0));  // toSheet(p) = offset + k·p (§6.5)
    Primitive p;
    p.kind = PrimitiveKind::Image;
    p.points = {img->lowerLeft + offset, img->upperRight + offset};
    p.image = img->image;
    p.role = role;
    out.push_back(std::move(p));
}

}  // namespace construe::drawing::detail
