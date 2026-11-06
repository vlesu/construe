// STL and 3MF mesh import (spec/io.md §4б; S4-MESH-001): parsing a file into triangle meshes, zip unpacking
// (stored and deflate RFC 1951 — own implementation, no external libraries).
#pragma once

#include "base/error.h"
#include "base/math.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace construe::io {

// Mesh from a file (io.md §4б): vertices welded by exact coordinate match, collapsed triangles
// (repeated vertex) skipped; triangles are counter-clockwise when viewed from outside (as in the file).
struct ImportedMesh {
    std::string name;  // 3MF — object name; ASCII STL — solid name; otherwise empty
    std::vector<Point3> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
};

enum class MeshFileKind { StlBinary, StlAscii, ThreeMf };

// File kind by content (io.md §4б item 1); not a mesh — IMPORT_MESH_FORMAT.
Result<MeshFileKind> detectMeshFile(const std::string& bytes);
// Parsing file content (pure function).
Result<std::vector<ImportedMesh>> parseMeshFile(const std::string& bytes);
// Reading a file (path is UTF-8): no file — IO_READ.
Result<std::vector<ImportedMesh>> readMeshFile(const std::string& path);

// Zip unpacking (io.md §4б item 3): parts in central directory order; methods 0 (stored) and 8 (deflate).
Result<std::vector<std::pair<std::string, std::string>>> unzip(const std::string& bytes);
// Inflating a raw deflate stream (RFC 1951) without a zlib header.
Result<std::string> inflateRaw(const std::string& data);

}  // namespace construe::io
