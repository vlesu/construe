// Solid from a mesh: segmentation, snapping, face description, building and check (spec/mesh-fit.md §3, §4.9).
#include "meshfit/meshfit_internal.h"

#include <algorithm>
#include <cmath>

namespace construe::meshfit {

namespace {

// Closed-shell check and fitting context (§4.1, §5).
Result<FitContext> makeContext(const Topo& topo, const Params& params) {
    for (const auto& n : topo.nbr)
        for (int k : n)
            if (k < 0) return Error("MESHFIT_NOT_CLOSED").with("open", 1.0).with("nonManifold", 0.0);
    FitContext c;
    c.topo = &topo;
    c.eps = params.distTol > 0 ? params.distTol : std::max(MESHFIT_ABS_TOL, MESHFIT_QUANT_REL * topo.maxCoord);
    c.sagitta = std::max(c.eps, MESHFIT_MAX_SAGITTA_REL * topo.diag);
    c.cosFacet = std::cos(MESHFIT_MAX_FACET_ANGLE);
    return c;
}

// Segmentation (§4.4) and snapping (§4.5) of recognized surfaces: a mesh face is exact on the plane of its triangle.
Result<std::vector<Seg>> snappedRegions(const FitContext& c, bool grid, const Cancel* cancel) {
    auto regs = segment(c, cancel);
    if (!regs) return regs.error();
    if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
    std::vector<Seg> canon;
    for (const auto& r : *regs)
        if (!r.mesh) canon.push_back(r);
    snapRegions(c, canon, grid);
    size_t k = 0;
    for (auto& r : *regs)
        if (!r.mesh) r = std::move(canon[k++]);
    return regs;
}

std::vector<Region> publicRegions(const Mesh& mesh, const Topo& topo, const std::vector<Seg>& regs) {
    std::vector<Region> out;
    for (const auto& r : regs) {
        Region g;
        g.surface = r.surface;
        g.triangles = r.tris;
        g.mesh = r.mesh;
        for (int v : regionVertices(topo, r.tris)) g.maxDev = std::max(g.maxDev, std::abs(signedDistance(r.surface, mesh.vertices[size_t(v)])));
        out.push_back(std::move(g));
    }
    return out;
}

}  // namespace

Result<std::vector<Region>> segmentMesh(const Mesh& mesh, const Params& params, const Cancel* cancel) {
    if (mesh.triangles.size() < 4) return Error("MESHFIT_NOT_CLOSED").with("open", 0.0).with("nonManifold", 0.0);
    const Topo topo = buildTopo(mesh);
    auto c = makeContext(topo, params);
    if (!c) return c.error();
    auto regs = snappedRegions(*c, params.snapGrid, cancel);
    if (!regs) return regs.error();
    return publicRegions(mesh, topo, *regs);
}

Result<SolidFit> fitSolid(const Mesh& mesh, const Params& params, const Cancel* cancel) {
    if (mesh.triangles.size() < 4) return Error("MESHFIT_NOT_CLOSED").with("open", 0.0).with("nonManifold", 0.0);
    const Topo topo = buildTopo(mesh);
    auto c = makeContext(topo, params);
    if (!c) return c.error();
    auto regs = snappedRegions(*c, params.snapGrid, cancel);
    if (!regs) return regs.error();
    auto spec = buildSolidSpec(*c, *regs);
    if (!spec) return spec.error();
    SolidFit out;
    out.solid = std::move(*spec);
    out.distTol = c->eps;
    out.regions = publicRegions(mesh, topo, *regs);
    for (const auto& r : out.regions) out.meshFaces += r.mesh ? 1 : 0;
    return out;
}

namespace {

Result<FittedSolid> buildOnce(const kernel::Kernel& k, const Mesh& mesh, const Params& params, const Cancel* cancel) {
    auto fit = fitSolid(mesh, params, cancel);
    if (!fit) return fit.error();
    auto shape = k.solidFromFaces(fit->solid, cancel);
    if (!shape) return Error("MESHFIT_BUILD_FAILED").with("detail", shape.error().stringParam("detail") ? *shape.error().stringParam("detail") : shape.error().code);
    // Check (§4.9): the solid is valid, its volume is close to the mesh volume, the mesh vertices lie on the solid.
    if (!k.checkValidity(*shape).valid || k.counts(*shape).solids != 1) return Error("MESHFIT_BUILD_FAILED").with("detail", std::string("invalid"));
    const double vm = meshVolume(mesh), vb = k.massProperties(*shape).volume;
    if (!(std::abs(vb - vm) <= MESHFIT_VOLUME_REL * std::abs(vm)))
        return Error("MESHFIT_BUILD_FAILED").with("detail", std::string("volume"));
    const size_t step = std::max<size_t>(1, mesh.vertices.size() / MESHFIT_CHECK_POINTS);
    double worst = 0;
    for (size_t i = 0; i < mesh.vertices.size(); i += step) worst = std::max(worst, k.distanceToShape(*shape, mesh.vertices[i]));
    if (!(worst <= MESHFIT_CHECK_DIST * fit->distTol)) return Error("MESHFIT_BUILD_FAILED").with("detail", std::string("deviation"));
    FittedSolid out;
    out.shape = *shape;
    out.fit = std::move(*fit);
    out.deviation = worst;
    return out;
}

}  // namespace

Result<FittedSolid> buildSolid(const kernel::Kernel& k, const Mesh& mesh, const Params& params, const Cancel* cancel) {
    auto first = buildOnce(k, mesh, params, cancel);
    if (first || !params.snapGrid || first.error().code == "KERNEL_CANCELLED" || cancelled(cancel)) return first;
    // Second attempt without dimensions on the grid (§4.9, FX10-STL-001): parts with non-round dimensions lose
    // tangency when two adjacent surfaces are rounded in opposite directions.
    if (first.error().code != "MESHFIT_BUILD_FAILED" && first.error().code != "MESHFIT_TOPOLOGY") return first;
    Params p = params;
    p.snapGrid = false;
    auto second = buildOnce(k, mesh, p, cancel);
    return second ? second : first;
}

}  // namespace construe::meshfit
