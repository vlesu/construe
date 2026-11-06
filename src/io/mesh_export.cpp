// Mesh export of assemblies and selected objects (spec/io.md §2а, §2б, §4а).
#include "io/mesh_export.h"

#include "base/number_format.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

namespace construe::io {

namespace {

// Vertex welding key: bitwise float32 values, −0 is normalized to 0 (io.md §2б item 1).
using VKey = std::array<uint32_t, 3>;
VKey keyOf(const float* p) {
    VKey k{};
    for (int i = 0; i < 3; ++i) {
        float f = p[i] == 0.0f ? 0.0f : p[i];
        std::memcpy(&k[size_t(i)], &f, 4);
    }
    return k;
}

// Welded mesh vertices: index of each source vertex → index of the welded one; welded positions are in order of first
// appearance while traversing the triangles.
struct Welded {
    std::vector<uint32_t> index;  // per source vertex (UINT32_MAX — not encountered)
    std::vector<const float*> points;
};

Welded weld(const BodyMesh& m) {
    Welded w;
    w.index.assign(m.positions.size() / 3, UINT32_MAX);
    std::map<VKey, uint32_t> seen;
    for (uint32_t vi : m.triangles) {
        if (w.index[vi] != UINT32_MAX) continue;
        const float* p = &m.positions[3 * size_t(vi)];
        auto [it, inserted] = seen.emplace(keyOf(p), uint32_t(w.points.size()));
        if (inserted) w.points.push_back(p);
        w.index[vi] = it->second;
    }
    return w;
}

double triArea(const float* a, const float* b, const float* c) {
    const Vec3 v0(a[0], a[1], a[2]), v1(b[0], b[1], b[2]), v2(c[0], c[1], c[2]);
    return (v1 - v0).cross(v2 - v0).norm() / 2;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

// Mesh positions transformed by the motion (float32 → double → motion → float32).
BodyMesh moved(const ExportMesh& e) {
    BodyMesh m = e.mesh;
    const auto& R = e.rotation;
    const auto& t = e.translation;
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
        const double x = m.positions[i], y = m.positions[i + 1], z = m.positions[i + 2];
        m.positions[i] = float(R[0] * x + R[1] * y + R[2] * z + t[0]);
        m.positions[i + 1] = float(R[3] * x + R[4] * y + R[5] * z + t[1]);
        m.positions[i + 2] = float(R[6] * x + R[7] * y + R[8] * z + t[2]);
    }
    std::string id;
    for (unsigned char c : e.mesh.bodyId) id.push_back(c >= 0x21 && c <= 0x7E ? char(c) : '_');
    m.bodyId = id.empty() ? std::string("_") : id;
    return m;
}

std::string model3mf(const std::vector<ExportMesh>& meshes) {
    // Colors — in order of first appearance (io.md §4а).
    std::vector<std::string> colors;
    auto colorIndex = [&](const std::string& c) {
        for (size_t i = 0; i < colors.size(); ++i)
            if (colors[i] == c) return int(i);
        colors.push_back(c);
        return int(colors.size() - 1);
    };
    for (const auto& e : meshes) {
        if (e.color) colorIndex(*e.color);
        for (const auto& fr : e.mesh.faceRanges)
            if (auto it = e.faceColors.find(fr.name); it != e.faceColors.end()) colorIndex(it->second);
    }
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    x += "<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">";
    x += "<metadata name=\"Application\">construe</metadata><resources>";
    if (!colors.empty()) {
        x += "<basematerials id=\"1\">";
        for (const auto& c : colors) x += "<base name=\"" + c + "\" displaycolor=\"" + c + "FF\"/>";
        x += "</basematerials>";
    }
    const int firstId = colors.empty() ? 1 : 2;
    for (size_t oi = 0; oi < meshes.size(); ++oi) {
        const ExportMesh& e = meshes[oi];
        x += "<object id=\"" + std::to_string(firstId + int(oi)) + "\" type=\"model\" name=\"" + xmlEscape(e.name) + "\"";
        if (e.color) x += " pid=\"1\" pindex=\"" + std::to_string(colorIndex(*e.color)) + "\"";
        x += "><mesh><vertices>";
        const Welded w = weld(e.mesh);
        for (const float* p : w.points)
            x += "<vertex x=\"" + formatStlNumber(p[0]) + "\" y=\"" + formatStlNumber(p[1]) + "\" z=\"" + formatStlNumber(p[2]) + "\"/>";
        x += "</vertices><triangles>";
        // Triangle color — by face (faceRanges).
        std::vector<int> triColor(e.mesh.triangles.size() / 3, -1);
        for (const auto& fr : e.mesh.faceRanges)
            if (auto it = e.faceColors.find(fr.name); it != e.faceColors.end())
                for (uint32_t t = fr.firstTriangle; t < fr.firstTriangle + fr.count && t < triColor.size(); ++t) triColor[t] = colorIndex(it->second);
        for (size_t t = 0; t * 3 + 2 < e.mesh.triangles.size(); ++t) {
            if (collapsedTriangle(&e.mesh.positions[3 * size_t(e.mesh.triangles[3 * t])], &e.mesh.positions[3 * size_t(e.mesh.triangles[3 * t + 1])],
                                  &e.mesh.positions[3 * size_t(e.mesh.triangles[3 * t + 2])]))
                continue;  // collapsed (io.md §4)
            x += "<triangle v1=\"" + std::to_string(w.index[e.mesh.triangles[3 * t]]) + "\" v2=\"" + std::to_string(w.index[e.mesh.triangles[3 * t + 1]]) +
                 "\" v3=\"" + std::to_string(w.index[e.mesh.triangles[3 * t + 2]]) + "\"";
            if (triColor[t] >= 0) x += " pid=\"1\" p1=\"" + std::to_string(triColor[t]) + "\"";
            x += "/>";
        }
        x += "</triangles></mesh></object>";
    }
    x += "</resources><build>";
    for (size_t oi = 0; oi < meshes.size(); ++oi) {
        const auto& R = meshes[oi].rotation;
        const auto& t = meshes[oi].translation;
        // Point is a row: x' = x·m00 + y·m10 + z·m20 + m30, i.e. m(i)(j) = R[j][i] (io.md §4а).
        const std::array<double, 12> m{R[0], R[3], R[6], R[1], R[4], R[7], R[2], R[5], R[8], t[0], t[1], t[2]};
        x += "<item objectid=\"" + std::to_string(firstId + int(oi)) + "\" transform=\"";
        for (size_t i = 0; i < m.size(); ++i) x += (i ? " " : "") + roundTrip(m[i]);
        x += "\"/>";
    }
    x += "</build></model>";
    return x;
}

void put16(std::string& s, uint32_t v) {
    s.push_back(char(v & 0xFF));
    s.push_back(char((v >> 8) & 0xFF));
}
void put32(std::string& s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

}  // namespace

uint32_t crc32(const std::string& bytes) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : bytes) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string zipStored(const std::vector<std::pair<std::string, std::string>>& parts) {
    std::string out, central;
    for (const auto& [name, data] : parts) {
        const uint32_t offset = uint32_t(out.size()), crc = crc32(data), size = uint32_t(data.size());
        put32(out, 0x04034B50);  // local file header
        put16(out, 20), put16(out, 0), put16(out, 0);  // version, flags, method (stored)
        put16(out, 0), put16(out, 0x0021);             // time, date 1980-01-01
        put32(out, crc), put32(out, size), put32(out, size);
        put16(out, uint32_t(name.size())), put16(out, 0);
        out += name;
        out += data;
        put32(central, 0x02014B50);  // central directory record
        put16(central, 20), put16(central, 20), put16(central, 0), put16(central, 0);
        put16(central, 0), put16(central, 0x0021);
        put32(central, crc), put32(central, size), put32(central, size);
        put16(central, uint32_t(name.size())), put16(central, 0), put16(central, 0);
        put16(central, 0), put16(central, 0), put32(central, 0), put32(central, offset);
        central += name;
    }
    const uint32_t cdOffset = uint32_t(out.size()), cdSize = uint32_t(central.size());
    out += central;
    put32(out, 0x06054B50);  // end of central directory
    put16(out, 0), put16(out, 0), put16(out, uint32_t(parts.size())), put16(out, uint32_t(parts.size()));
    put32(out, cdSize), put32(out, cdOffset), put16(out, 0);
    return out;
}

std::vector<MeshCheck> checkMeshes(const std::vector<ExportMesh>& meshes) {
    std::vector<MeshCheck> out;
    for (const auto& e : meshes) {
        MeshCheck c;
        c.name = e.name;
        const BodyMesh& m = e.mesh;
        c.triangles = uint32_t(m.triangles.size() / 3);
        const Welded w = weld(m);
        std::map<std::pair<uint32_t, uint32_t>, std::pair<int, int>> edges;  // {min,max} → (count min→max, max→min)
        for (size_t t = 0; t * 3 + 2 < m.triangles.size(); ++t) {
            if (collapsedTriangle(&m.positions[3 * size_t(m.triangles[3 * t])], &m.positions[3 * size_t(m.triangles[3 * t + 1])],
                                  &m.positions[3 * size_t(m.triangles[3 * t + 2])])) {
                --c.triangles;  // collapsed — skipped (io.md §2б item 0)
                continue;
            }
            const uint32_t v[3] = {w.index[m.triangles[3 * t]], w.index[m.triangles[3 * t + 1]], w.index[m.triangles[3 * t + 2]]};
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = v[k], b = v[(k + 1) % 3];
                if (a == b) continue;
                auto& cnt = edges[{std::min(a, b), std::max(a, b)}];
                (a < b ? cnt.first : cnt.second)++;
            }
            const float* p[3] = {&m.positions[3 * size_t(m.triangles[3 * t])], &m.positions[3 * size_t(m.triangles[3 * t + 1])],
                                 &m.positions[3 * size_t(m.triangles[3 * t + 2])]};
            if (triArea(p[0], p[1], p[2]) < STL_DEGENERATE_AREA) ++c.degenerate;
        }
        for (const auto& [key, cnt] : edges) {
            const int total = cnt.first + cnt.second;
            if (total == 1) ++c.openEdges;
            else if (total == 2) c.flippedEdges += cnt.first != 1 ? 1 : 0;
            else ++c.nonManifoldEdges;
        }
        out.push_back(c);
    }
    return out;
}

Result<std::string> meshFileBytes(const std::vector<ExportMesh>& meshes, const MeshFileOptions& options) {
    if (meshes.empty()) return Error("EXPORT_NO_BODIES");
    if (options.fileFormat == MeshFileFormat::Stl) {
        std::vector<BodyMesh> ms;
        for (const auto& e : meshes) ms.push_back(moved(e));
        return stlBytes(ms, options.stlFormat);
    }
    const std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>";
    return zipStored({{"[Content_Types].xml", types}, {"_rels/.rels", rels}, {"3D/3dmodel.model", model3mf(meshes)}});
}

Status writeMeshFile(const std::string& path, const std::vector<ExportMesh>& meshes, const MeshFileOptions& options) {
    auto bytes = meshFileBytes(meshes, options);
    if (!bytes) return bytes.error();
    return writeFileAtomic(path, *bytes);
}

}  // namespace construe::io
