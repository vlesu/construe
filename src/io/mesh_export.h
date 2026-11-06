// Mesh export of assemblies and selected objects: STL and 3MF, mesh check (spec/io.md §2а, §2б, §4а).
#pragma once

#include "io/stl.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::io {

// Exported mesh (io.md §2а): body mesh in part coordinates and its world placement.
struct ExportMesh {
    std::string name;
    BodyMesh mesh;
    std::array<double, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major
    std::array<double, 3> translation{0, 0, 0};
    std::optional<std::string> color;                      // #RRGGBB
    std::map<std::string, std::string> faceColors;         // face name → #RRGGBB
};

enum class MeshFileFormat { Stl, ThreeMf };

struct MeshFileOptions {
    MeshFileFormat fileFormat = MeshFileFormat::Stl;
    StlFormat stlFormat = StlFormat::Binary;
};

// Mesh check (io.md §2б).
struct MeshCheck {
    std::string name;
    uint32_t triangles = 0;
    uint32_t openEdges = 0, flippedEdges = 0, nonManifoldEdges = 0, degenerate = 0;
    bool ok() const { return openEdges == 0 && flippedEdges == 0 && nonManifoldEdges == 0 && degenerate == 0; }
};

std::vector<MeshCheck> checkMeshes(const std::vector<ExportMesh>& meshes);

// File content (pure functions) and atomic write.
Result<std::string> meshFileBytes(const std::vector<ExportMesh>& meshes, const MeshFileOptions& options);
Status writeMeshFile(const std::string& path, const std::vector<ExportMesh>& meshes, const MeshFileOptions& options);

// Uncompressed zip package (io.md §4а): parts in the given order.
std::string zipStored(const std::vector<std::pair<std::string, std::string>>& parts);
uint32_t crc32(const std::string& bytes);

}  // namespace construe::io
