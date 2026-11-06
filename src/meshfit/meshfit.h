// Surface recognition from a triangle mesh and solid description from surfaces (spec/mesh-fit.md; S4-MESH-001):
// segmentation by fitting (plane, cylinder, cone, sphere, torus), parameter refinement, snapping to "round"
// values, edges from intersections of adjacent surfaces. Pure functions; from the kernel only body description types.
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "base/math.h"
#include "kernel/kernel.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace construe::meshfit {

using kernel::SurfaceSpec;
using Kind = kernel::SurfaceInfo::Kind;

// Mesh (mesh-fit.md §2.1): vertices and triangles, counterclockwise when viewed from outside.
struct Mesh {
    std::vector<Point3> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
};

// ---------------------------------------------------------------- surfaces (§4.1)

// Signed distance from a point to the surface (§4.1): > 0 along the surface normal.
double signedDistance(const SurfaceSpec& s, const Point3& p);
// Unit surface normal at the point closest to p (distance gradient).
Vec3 surfaceNormal(const SurfaceSpec& s, const Point3& p);
// Projection of a point onto the surface.
Point3 projectToSurface(const SurfaceSpec& s, const Point3& p);
// Number of parameters of a kind (§4.2): plane 3, sphere 4, cylinder 5, cone 6, torus 7.
int parameterCount(Kind k);

struct FitResult {
    bool ok = false;
    SurfaceSpec surface;
    double rms = 0, maxDev = 0;  // over the fitted points
};

// Fitting a kind to points (§4.2–4.3): initial guess (from points and normals) and least squares of geometric
// distances (Levenberg–Marquardt). normals: normal estimates at the points (same length) or empty.
FitResult fitSurface(Kind kind, const std::vector<Point3>& points, const std::vector<Vec3>& normals);
// Refining a given surface from points (§4.3).
FitResult refineSurface(const SurfaceSpec& start, const std::vector<Point3>& points);

// ---------------------------------------------------------------- solid from a mesh (§3)

// Recognition time limit of one shell in mesh import (§4.10; FX10-STL-001), seconds: beyond it the import keeps the
// polyhedral body (reason MESHFIT_TIMEOUT).
// A Debug build is about ten times slower; the same mesh must get the same result there (mesh-fit.md §4.10).
#ifdef CONSTRUE_OPTIMIZED_BUILD
inline constexpr double MESHFIT_TIME_LIMIT = 120;
#else
inline constexpr double MESHFIT_TIME_LIMIT = 960;
#endif

struct Params {
    double distTol = 0;  // mm: tolerance of vertex-to-surface distance; 0 means automatic (§5)
    bool snapGrid = true;  // dimensions on the MESHFIT_SNAP_GRID grid (§4.5 C); buildSolid retries without them (§4.9)
};

struct Region {
    SurfaceSpec surface;
    std::vector<int> triangles;  // ascending
    double maxDev = 0;           // mm: largest deviation of region vertices from the surface after snapping
    bool mesh = false;           // mesh face: one unrecognized triangle (free-form part of a mixed solid, S4-MESH-002)
};

struct SolidFit {
    kernel::SolidSpec solid;
    std::vector<Region> regions;  // region i is face i of the description
    double distTol = 0;           // tolerance applied
    int meshFaces = 0;            // mesh faces (free-form parts kept as triangles, S4-MESH-002); 0 — all surfaces recognized
};

// Regions of a closed mesh of one connected shell after segmentation and snapping (§3, §4.4–4.5), without topology:
// diagnostics and quality measurement (§11); MESHFIT_* errors of these steps (§7).
Result<std::vector<Region>> segmentMesh(const Mesh& mesh, const Params& params = {}, const Cancel* cancel = nullptr);

// Solid description from a closed mesh of one connected shell (§3): MESHFIT_* errors (§7).
Result<SolidFit> fitSolid(const Mesh& mesh, const Params& params = {}, const Cancel* cancel = nullptr);

// Kernel solid from a shell mesh (§3, §4.9): fitSolid → kernel.solidFromFaces → match check against the mesh.
struct FittedSolid {
    kernel::Shape shape;
    SolidFit fit;
    double deviation = 0;  // mm: largest distance of the checked mesh vertices to the solid
};
Result<FittedSolid> buildSolid(const kernel::Kernel& k, const Mesh& mesh, const Params& params = {}, const Cancel* cancel = nullptr);

// Mesh preparation (§4.0): welding coincident vertices, removing collapsed triangles, splitting into connected
// shells; error MESHFIT_NOT_CLOSED: the mesh is not closed or not manifold.
Result<std::vector<Mesh>> splitShells(const Mesh& mesh);
// Volume enclosed by a closed mesh (signed: > 0 for outward orientation).
double meshVolume(const Mesh& mesh);

}  // namespace construe::meshfit
