// Internal declarations of the meshfit module (spec/mesh-fit.md).
#pragma once

#include "meshfit/meshfit.h"

#include <optional>

namespace construe::meshfit {

// ---------------------------------------------------------------- constants (§5)

inline constexpr double MESHFIT_ABS_TOL = 1e-4;          // mm: lower bound of the vertex-to-surface distance tolerance
inline constexpr double MESHFIT_QUANT_REL = 2e-6;        // fraction of max|coord|: margin for float32 rounding in the file
inline constexpr double MESHFIT_MAX_FACET_ANGLE = 0.4;   // rad: angle of the triangle normal to the surface normal
inline constexpr double MESHFIT_MAX_RADIUS_REL = 100;    // ×bbox diagonal: largest radius (cone apex no farther)
inline constexpr double MESHFIT_MAX_STEP_ANGLE = 0.6;    // rad: angle between normals of adjacent triangles of a curved surface
inline constexpr double MESHFIT_MAX_SAGITTA_REL = 0.01;  // fraction of the bbox diagonal: triangle center deviation
inline constexpr size_t MESHFIT_LM_MAX_POINTS = 4000;    // points in a fit (uniform sampling)
inline constexpr int MESHFIT_LM_MAX_ITER = 100;          // Levenberg–Marquardt iterations
inline constexpr size_t MESHFIT_NEIGHBORHOOD = 40;       // triangles in a seed neighborhood
inline constexpr size_t MESHFIT_RANSAC_PAIRS = 12;       // leading neighborhood triangles for pairs (second from the first 2K)
inline constexpr size_t MESHFIT_RANSAC_CONE_FIRST = 3;   // leading neighborhood triangles: the first in cone triples
inline constexpr size_t MESHFIT_MAX_UNION_TRIES = 400;
inline constexpr size_t MESHFIT_MAX_SEEDS = 2000;        // segmentation seeds: then the remaining triangles are not seeded (§4.4 item 1)
inline constexpr size_t MESHFIT_MAX_MESH_FACES = 5000;   // mesh faces of a mixed solid (§4.4 item 8, S4-MESH-002)
inline constexpr double MESHFIT_SLIVER_FACTOR = 2;       // sliver: height ≤ this × the edge midpoint deviation (§4.4 item 7ж)
inline constexpr int MESHFIT_MIN_EXTRA_VERTICES = 3;     // curved region: vertices beyond the parameter count (§4.4 item 7г)
inline constexpr double MESHFIT_SNAP_GRID = 1e-3;        // mm: dimension snapping step
inline constexpr double MESHFIT_SNAP_ANGLE = 1e-3;       // rad: snapping directions to axes and to each other
inline constexpr double MESHFIT_ANGLE_GRID = 0.5;        // degrees: cone angle snapping step
inline constexpr double MESHFIT_SNAP_CHECK = 3;          // ×tolerance: region vertices after snapping
inline constexpr double MESHFIT_SNAP_NOISE = 3;          // ×the region's own fit deviation: region vertices after snapping
inline constexpr double MESHFIT_SNAP_QUANT_REL = 4e-7;   // ×max|coord|: float32 quantization floor of that bound
inline constexpr size_t MESHFIT_INSIDE_CANDIDATES = 200;  // candidate triangles for the inside point of a face
inline constexpr double MESHFIT_VERTEX_TOL = 1e-3;       // mm: solid vertex residual (surface intersection)
inline constexpr double MESHFIT_SEW_TOL = 1e-4;          // mm: face sewing tolerance in the kernel
inline constexpr double MESHFIT_VOLUME_REL = 0.03;       // relative volume mismatch between solid and mesh (check)
inline constexpr size_t MESHFIT_CHECK_POINTS = 400;      // mesh vertices in the deviation check
inline constexpr double MESHFIT_CHECK_DIST = 10;         // ×tolerance: deviation of a mesh vertex from the solid

// ---------------------------------------------------------------- mesh with adjacency (§4.0)

struct Topo {
    const Mesh* mesh = nullptr;
    std::vector<std::array<int, 3>> nbr;  // adjacent triangle across edge k = (v[k], v[k+1])
    std::vector<Vec3> normal;             // triangle normals
    std::vector<double> area;
    std::vector<Point3> centroid;
    double diag = 0, maxCoord = 0;
};
Topo buildTopo(const Mesh& m);  // precondition: the mesh is closed and consistently oriented (splitShells)

// ---------------------------------------------------------------- segmentation (§4.4)

struct Seg {
    SurfaceSpec surface;
    std::vector<int> tris;  // ascending
    int sign = 1;           // sign of the triangle normals relative to the surface normal
    bool mesh = false;      // mesh face: one unrecognized triangle on its own plane (§4.4 item 8, S4-MESH-002)
};

struct FitContext {
    const Topo* topo = nullptr;
    double eps = 0;       // distance tolerance
    double sagitta = 0;   // triangle center deviation tolerance
    double cosFacet = 0;  // cos MESHFIT_MAX_FACET_ANGLE
};

bool plausible(const FitContext& c, const SurfaceSpec& s);
bool consistent(const FitContext& c, const SurfaceSpec& s, int sign, int t);
bool smoothStep(const FitContext& c, const SurfaceSpec& s, int a, int b);
bool regionConsistent(const FitContext& c, const SurfaceSpec& s, int sign, const std::vector<int>& tris);
int facetSign(const FitContext& c, const SurfaceSpec& s, int t);
std::vector<int> regionVertices(const Topo& t, const std::vector<int>& tris);
// Kind simplicity order (§4.4): plane, cylinder, cone, sphere, torus; when indistinguishable, the simpler one is chosen.
int simplicity(Kind k);
double facetDeviation(const FitContext& c, const SurfaceSpec& s, const std::vector<int>& tris);
Result<std::vector<Seg>> segment(const FitContext& c, const Cancel* cancel);

// ---------------------------------------------------------------- snapping (§4.5)

// grid = false (§4.9 second attempt, FX10-STL-001): directions and shared axes only, no dimensions on the grid.
void snapRegions(const FitContext& c, std::vector<Seg>& regions, bool grid = true);

// ---------------------------------------------------------------- topology and edge geometry (§4.6–4.8)

Result<kernel::SolidSpec> buildSolidSpec(const FitContext& c, const std::vector<Seg>& regions);

SurfaceSpec canonical(const SurfaceSpec& s);

// Refinement with fixed parameters (§4.5): Dir fixes the direction; Axis the axis (cone and torus keep the shift along
// the axis and the dimensions); AxisAngle leaves the cone only the apex shift along the axis.
enum class Lock { Dir, Axis, AxisAngle };
// Initial guess of a kind without refinement (§4.2).
std::optional<SurfaceSpec> initialSurface(Kind kind, const std::vector<Point3>& pts, const std::vector<Vec3>& nrm);
FitResult refineLocked(const SurfaceSpec& start, const std::vector<Point3>& pts, Lock lock);

}  // namespace construe::meshfit
