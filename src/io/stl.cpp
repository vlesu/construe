// STL export (spec/io.md).
#include "io/stl.h"

#include "base/files.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>

namespace construe::io {

Status validateOptions(const StlOptions& o) {
    auto bad = [](const char* name, double actual) {
        return Status(Error("EXPORT_INVALID_OPTIONS").with("name", std::string(name)).with("actual", actual));
    };
    if (o.chordalDeviation) {
        const double c = *o.chordalDeviation;
        if (!(c >= kernel::KERNEL_MIN_CHORD && c <= kernel::KERNEL_MAX_CHORD)) return bad("chordalDeviation", c);
    }
    if (o.angularDeviation) {
        const double a = *o.angularDeviation;
        if (!(a >= kernel::KERNEL_MIN_ANGLE_TOL && a <= std::numbers::pi / 2)) return bad("angularDeviation", a);
    }
    if (!o.allBodies) {
        std::set<std::string> seen;
        bool dup = false;
        for (const auto& b : o.bodies) dup = dup || !seen.insert(b).second;
        if (o.bodies.empty() || dup)
            return Status(Error("EXPORT_INVALID_OPTIONS").with("name", std::string("bodies")).with("actual", o.bodies));
    }
    return okStatus();
}

kernel::TessParams tessParams(const StlOptions& o, const BBox3& bb) {
    kernel::TessParams p;
    if (o.chordalDeviation) {
        p.chordTol = *o.chordalDeviation;
    } else {
        const double d = bb.isEmpty() ? 0.0 : bb.diagonal().norm();
        p.chordTol = std::clamp(std::max(STL_MIN_CHORD, STL_REL_CHORD * d), kernel::KERNEL_MIN_CHORD, kernel::KERNEL_MAX_CHORD);
    }
    p.angleTol = o.angularDeviation.value_or(STL_ANGULAR);
    return p;
}

std::string formatStlNumber(float v) {
    double d = static_cast<double>(v);
    if (d == 0.0) d = 0.0;  // -0 → 0
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", d);
    // Locale independence: replace a possible comma.
    for (char* p = buf; *p; ++p)
        if (*p == ',') *p = '.';
    // MSVC prints the exponent with three digits in old modes — normalize to at least two.
    std::string s = buf;
    const auto e = s.find('e');
    if (e != std::string::npos) {
        std::string mant = s.substr(0, e);
        char sign = s[e + 1];
        std::string digits = s.substr(e + 2);
        while (digits.size() > 2 && digits[0] == '0') digits.erase(0, 1);
        s = mant + "e" + sign + digits;
    }
    return s;
}

namespace {

std::array<float, 3> triNormal(const float* a, const float* b, const float* c) {
    const Vec3 v0(a[0], a[1], a[2]), v1(b[0], b[1], b[2]), v2(c[0], c[1], c[2]);
    const Vec3 n = (v1 - v0).cross(v2 - v0);
    const double len = n.norm();
    if (len / 2 < STL_DEGENERATE_AREA) return {0.f, 0.f, 0.f};
    return {static_cast<float>(n.x() / len), static_cast<float>(n.y() / len), static_cast<float>(n.z() / len)};
}

void putF(std::string& out, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}

}  // namespace

bool collapsedTriangle(const float* a, const float* b, const float* c) {
    auto same = [](const float* p, const float* q) {
        for (int i = 0; i < 3; ++i)
            if (!(p[i] == q[i])) return false;  // -0 == 0 in float comparison
        return true;
    };
    return same(a, b) || same(b, c) || same(a, c);
}

Result<std::string> stlBytes(const std::vector<BodyMesh>& meshes, StlFormat format) {
    if (meshes.empty()) return Error("EXPORT_NO_BODIES");
    std::string out;
    if (format == StlFormat::Binary) {
        std::string header = "construe STL";
        header.resize(80, ' ');
        out = header;
        uint32_t n = 0;
        for (const auto& m : meshes)
            for (size_t t = 0; t + 2 < m.triangles.size(); t += 3)
                if (!collapsedTriangle(&m.positions[3 * size_t(m.triangles[t])], &m.positions[3 * size_t(m.triangles[t + 1])],
                                       &m.positions[3 * size_t(m.triangles[t + 2])]))
                    ++n;
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
        out.reserve(84 + 50 * size_t(n));
        for (const auto& m : meshes) {
            for (size_t t = 0; t + 2 < m.triangles.size(); t += 3) {
                const float* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &m.positions[3 * size_t(m.triangles[t + k])];
                if (collapsedTriangle(v[0], v[1], v[2])) continue;  // collapsed (io.md §4)
                const auto nn = triNormal(v[0], v[1], v[2]);
                for (float f : nn) putF(out, f);
                for (int k = 0; k < 3; ++k)
                    for (int j = 0; j < 3; ++j) putF(out, v[k][j]);
                out.push_back(0);
                out.push_back(0);
            }
        }
        return out;
    }
    for (const auto& m : meshes) {
        out += "solid " + m.bodyId + "\n";
        for (size_t t = 0; t + 2 < m.triangles.size(); t += 3) {
            const float* v[3];
            for (int k = 0; k < 3; ++k) v[k] = &m.positions[3 * size_t(m.triangles[t + k])];
            if (collapsedTriangle(v[0], v[1], v[2])) continue;  // collapsed (io.md §4)
            const auto nn = triNormal(v[0], v[1], v[2]);
            out += "  facet normal " + formatStlNumber(nn[0]) + " " + formatStlNumber(nn[1]) + " " + formatStlNumber(nn[2]) + "\n";
            out += "    outer loop\n";
            for (int k = 0; k < 3; ++k)
                out += "      vertex " + formatStlNumber(v[k][0]) + " " + formatStlNumber(v[k][1]) + " " + formatStlNumber(v[k][2]) + "\n";
            out += "    endloop\n";
            out += "  endfacet\n";
        }
        out += "endsolid " + m.bodyId + "\n";
    }
    return out;
}

Status writeFileAtomic(const std::string& path, const std::string& bytes) {
    namespace fs = std::filesystem;
    const fs::path target = u8path(path);
    fs::path tmp = target;
    tmp += u8".construe-tmp";
    auto fail = [&](const std::string& detail) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return Status(Error("IO_WRITE").with("detail", detail).with("file", path));
    };
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot open for writing");
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) return fail("write failed");
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) return fail(ec.message());
    return okStatus();
}

Status writeStl(const std::string& path, const std::vector<BodyMesh>& meshes, StlFormat format) {
    auto bytes = stlBytes(meshes, format);
    if (!bytes) return bytes.error();
    return writeFileAtomic(path, *bytes);
}

}  // namespace construe::io
