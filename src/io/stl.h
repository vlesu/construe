// STL export (spec/io.md).
#pragma once

#include "base/error.h"
#include "base/math.h"
#include "kernel/kernel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace construe::io {

inline constexpr double STL_MIN_CHORD = 0.001;
inline constexpr double STL_REL_CHORD = 1e-4;
inline constexpr double STL_ANGULAR = 0.1;
inline constexpr double STL_DEGENERATE_AREA = 1e-12;

enum class StlFormat { Binary, Ascii };

// Body mesh in the api layout (spec/core-api.md §5.2).
struct FaceRange {
    std::string name;
    uint32_t firstTriangle = 0;
    uint32_t count = 0;
};
struct EdgePolylines {
    std::string name;
    std::vector<std::vector<float>> polylines;
};
struct VertexPos {
    std::string name;
    std::array<float, 3> position{};
};
struct BodyMesh {
    std::string bodyId;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> triangles;
    std::vector<FaceRange> faceRanges;
    std::vector<EdgePolylines> edges;
    std::vector<VertexPos> vertices;
    std::vector<std::string> failedFaces;
    double chordTol = 0;  // chordal tolerance the mesh was built with (mm; 0 — unknown), core-api §5.2
};

struct StlOptions {
    StlFormat format = StlFormat::Binary;
    std::optional<double> chordalDeviation;
    std::optional<double> angularDeviation;
    bool allBodies = true;
    std::vector<std::string> bodies;
};

Status validateOptions(const StlOptions& o);
kernel::TessParams tessParams(const StlOptions& o, const BBox3& bodiesBBox);

// File content (pure function) and atomic write.
Result<std::string> stlBytes(const std::vector<BodyMesh>& meshes, StlFormat format);
Status writeStl(const std::string& path, const std::vector<BodyMesh>& meshes, StlFormat format);

// Atomic file write (temporary file + rename); path is UTF-8.
Status writeFileAtomic(const std::string& path, const std::string& bytes);

// Collapsed triangle (io.md §4): two vertices coincide bitwise (float32, -0 = 0) — skipped on write and check.
bool collapsedTriangle(const float* a, const float* b, const float* c);

// ASCII STL number format: %.9g in the "C" locale, -0 → 0.
std::string formatStlNumber(float v);

}  // namespace construe::io
