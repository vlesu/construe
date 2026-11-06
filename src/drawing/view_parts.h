// Internal parts of view construction (spec/drawing.md §6.11–6.16): hatching, circle clipping, designations, center
// lines, threads. Shared by views.cpp, sections.cpp, view_decor.cpp; not part of the module's public interface.
#pragma once
#include "drawing/sheet.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace construe::drawing::detail {

// View frame (§6.1): n — toward the viewer, u — up, r = u × n — right.
struct Frame {
    Vec3 n, u, r;
};

// Section cutting plane (§6.11): through origin, normal — toward the viewer (the removed side).
struct Cut {
    Point3 origin = Point3::Zero();
    Vec3 normal = Vec3::UnitZ();
    bool sectionOnly = false;
};

using ToSheet = std::function<Point2(const Point2&)>;

Primitive makeLine(const Point2& a, const Point2& b, LineType lt, Weight w, const std::string& role);

// Shaded image of the view bodies (§6.22, FX8-DRW-003, view_render.cpp): the tessellation drawn into a raster image —
// one image primitive; colors — per body ("#RRGGBB", "" — RENDER_DEFAULT_COLOR).
void appendShading(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies, const std::vector<std::string>& colors,
                   const Frame& f, const ToSheet& toSheet, double scale, const std::string& role, std::vector<Primitive>& out);
// Triangle of the shaded image (§6.22): sheet points, depth toward the viewer, normals in the view frame (z — toward the
// viewer), color channels 0…255.
struct ShadedTriangle {
    Point2 p[3];
    double depth[3] = {0, 0, 0};
    Vec3 normal[3];
    std::array<double, 3> color{};
};
struct ShadedImage {
    Point2 lowerLeft{0, 0}, upperRight{0, 0};
    std::shared_ptr<const RasterImage> image;
};
// Depth-buffer rasterization of the triangles at dpi (§6.22); no triangles — nullopt.
std::optional<ShadedImage> rasterizeShaded(const std::vector<ShadedTriangle>& tris, double dpi);
Primitive makeText(const Point2& at, const std::string& text, double height, const std::string& align, const std::string& role);

// Hatching of section figures (§6.14): figures — in view coordinates, converted by toSheet; pos — effective view position.
void appendHatch(const std::vector<kernel::SectionRegion>& regions, const ToSheet& toSheet, const Point2& pos, const std::string& role,
                 std::vector<Primitive>& out);
// Figure area: outer contour minus holes (by absolute values of signed areas).
double regionArea(const std::vector<std::vector<Point2>>& loops);

// Clipping a primitive by a circle (§6.12): parts inside the circle (points on the circle — inside).
void clipToCircle(const Primitive& p, const Point2& c, double R, std::vector<Primitive>& out);

// Bounding box of primitives (circles and arcs — by the full circle, texts — by the anchor point).
Eigen::AlignedBox2d primitivesBox(const std::vector<Primitive>& prims);

// Center lines and center marks (§6.15) from the view bodies.
void appendCenterLines(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies, const Frame& f, const ToSheet& toSheet,
                       double scale, const std::optional<Cut>& cut, const std::string& role, std::vector<Primitive>& out);

// Thread representation (§6.16). visible — visible curves of the view projection (view coordinates) for end-face visibility.
void appendThreads(const std::vector<regen::ThreadEntry>& threads, const Frame& f, const ToSheet& toSheet, double scale,
                   const std::optional<Cut>& cut, bool hiddenLines, const std::vector<kernel::ViewCurve>* visible, const std::string& role,
                   std::vector<Primitive>& out);

// Gear rims (§6.20): view bodies with the spaces filled up to the tip circle; pitch and root lines.
std::vector<kernel::PlacedShape> gearConventionalBodies(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies,
                                                        const std::vector<regen::GearEntry>& gears);
void appendGears(const std::vector<regen::GearEntry>& gears, const Frame& f, const ToSheet& toSheet, double scale,
                 const std::optional<Cut>& cut, const std::string& role, std::vector<Primitive>& out);

// Scale text for a label (§6.13): point → comma.
std::string scaleText(const std::string& scale);

}  // namespace construe::drawing::detail
