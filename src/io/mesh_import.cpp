// STL and 3MF mesh import (spec/io.md §4б).
#include "io/mesh_import.h"

#include "io/mesh_export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>

namespace construe::io {

namespace {

Error formatError(const std::string& detail) { return Error("IMPORT_MESH_FORMAT").with("detail", detail); }

uint32_t get32(const std::string& b, size_t at) {
    return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8 | uint32_t(uint8_t(b[at + 2])) << 16 |
           uint32_t(uint8_t(b[at + 3])) << 24;
}
uint32_t get16(const std::string& b, size_t at) { return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8; }

// Vertex welding by exact coordinate match (io.md §4б item 2); −0 = 0.
struct Welder {
    std::map<std::array<double, 3>, uint32_t> index;
    ImportedMesh mesh;
    uint32_t add(const Point3& p) {
        std::array<double, 3> k{p.x() == 0 ? 0.0 : p.x(), p.y() == 0 ? 0.0 : p.y(), p.z() == 0 ? 0.0 : p.z()};
        auto [it, inserted] = index.emplace(k, uint32_t(mesh.vertices.size()));
        if (inserted) mesh.vertices.push_back({k[0], k[1], k[2]});
        return it->second;
    }
    void tri(uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || a == c) return;  // collapsed
        mesh.triangles.push_back({a, b, c});
    }
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

Result<std::vector<ImportedMesh>> parseBinaryStl(const std::string& b) {
    const uint32_t n = get32(b, 80);
    Welder w;
    for (uint32_t t = 0; t < n; ++t) {
        const size_t at = 84 + 50 * size_t(t) + 12;
        uint32_t idx[3];
        for (int v = 0; v < 3; ++v) {
            float f[3];
            std::memcpy(f, b.data() + at + 12 * size_t(v), 12);
            for (float x : f)
                if (!std::isfinite(x)) return formatError("nan");
            idx[v] = w.add({f[0], f[1], f[2]});
        }
        w.tri(idx[0], idx[1], idx[2]);
    }
    std::vector<ImportedMesh> out;
    out.push_back(std::move(w.mesh));
    return out;
}

Result<std::vector<ImportedMesh>> parseAsciiStl(const std::string& b) {
    std::vector<ImportedMesh> out;
    size_t i = 0;
    auto word = [&]() {
        while (i < b.size() && isSpace(b[i])) ++i;
        const size_t s = i;
        while (i < b.size() && !isSpace(b[i])) ++i;
        return b.substr(s, i - s);
    };
    auto restOfLine = [&]() {
        while (i < b.size() && (b[i] == ' ' || b[i] == '\t')) ++i;
        const size_t s = i;
        while (i < b.size() && b[i] != '\n' && b[i] != '\r') ++i;
        std::string r = b.substr(s, i - s);
        while (!r.empty() && isSpace(r.back())) r.pop_back();
        return r;
    };
    auto number = [&](double& v) {
        const std::string w = word();
        if (w.empty()) return false;
        std::istringstream ss(w);
        ss.imbue(std::locale::classic());
        ss >> v;
        return !ss.fail() && std::isfinite(v);
    };
    while (true) {
        const std::string kw = word();
        if (kw.empty()) break;
        if (kw != "solid") return formatError("ascii");
        Welder w;
        w.mesh.name = restOfLine();
        while (true) {
            const std::string t = word();
            if (t == "endsolid") {
                restOfLine();
                break;
            }
            if (t != "facet") return formatError("ascii");
            double nv;
            if (word() != "normal" || !number(nv) || !number(nv) || !number(nv)) return formatError("ascii");
            if (word() != "outer" || word() != "loop") return formatError("ascii");
            uint32_t idx[3];
            for (auto& id : idx) {
                double x, y, z;
                if (word() != "vertex" || !number(x) || !number(y) || !number(z)) return formatError("ascii");
                // ASCII STL values are float32 (io.md §4): rounding aligns with the binary write of the same mesh.
                id = w.add({double(float(x)), double(float(y)), double(float(z))});
            }
            if (word() != "endloop" || word() != "endfacet") return formatError("ascii");
            w.tri(idx[0], idx[1], idx[2]);
        }
        out.push_back(std::move(w.mesh));
    }
    if (out.empty()) return formatError("ascii");
    return out;
}

// ------------------------------------------------------------ deflate (RFC 1951)

struct BitReader {
    const std::string& d;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool overrun = false;
    explicit BitReader(const std::string& s) : d(s) {}
    int bit() {
        if (bitCnt == 0) {
            if (pos >= d.size()) {
                overrun = true;
                return 0;
            }
            bitBuf = uint8_t(d[pos++]);
            bitCnt = 8;
        }
        const int b = int(bitBuf & 1u);
        bitBuf >>= 1;
        --bitCnt;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int k = 0; k < n; ++k) v |= uint32_t(bit()) << k;
        return v;
    }
};

// Canonical Huffman code (RFC 1951 §3.2.2): number of codes of each length and symbols in increasing code order.
struct Huffman {
    std::array<int, 16> count{};
    std::vector<int> symbol;
    bool build(const int* lengths, int n) {
        count.fill(0);
        for (int s = 0; s < n; ++s) ++count[size_t(lengths[s])];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[size_t(len)];
            if (left < 0) return false;  // over-subscribed code
        }
        std::array<int, 16> offs{};
        for (int len = 1; len < 15; ++len) offs[size_t(len + 1)] = offs[size_t(len)] + count[size_t(len)];
        symbol.assign(size_t(n), 0);
        for (int s = 0; s < n; ++s)
            if (lengths[s] != 0) symbol[size_t(offs[size_t(lengths[s])]++)] = s;
        return true;
    }
    int decode(BitReader& r) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= r.bit();
            const int c = count[size_t(len)];
            if (code - c < first) return symbol[size_t(index + (code - first))];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
            if (r.overrun) return -1;
        }
        return -1;
    }
};

constexpr int LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& r, std::string& out, const Huffman& lit, const Huffman& dist) {
    while (true) {
        const int sym = lit.decode(r);
        if (sym < 0 || r.overrun) return false;
        if (sym < 256) {
            out.push_back(char(sym));
            continue;
        }
        if (sym == 256) return true;
        const int li = sym - 257;
        if (li >= 29) return false;
        const int len = LEN_BASE[li] + int(r.bits(LEN_EXTRA[li]));
        const int ds = dist.decode(r);
        if (ds < 0 || ds >= 30) return false;
        const size_t d = size_t(DIST_BASE[ds]) + r.bits(DIST_EXTRA[ds]);
        if (d > out.size() || r.overrun) return false;
        const size_t from = out.size() - d;
        for (int k = 0; k < len; ++k) out.push_back(out[from + size_t(k)]);
    }
}

}  // namespace

Result<std::string> inflateRaw(const std::string& data) {
    BitReader r(data);
    std::string out;
    int last = 0;
    while (!last) {
        last = r.bit();
        const uint32_t type = r.bits(2);
        if (r.overrun) return formatError("deflate");
        if (type == 0) {  // stored: byte alignment, LEN, NLEN
            r.bitCnt = 0;
            if (r.pos + 4 > data.size()) return formatError("deflate");
            const uint32_t len = get16(data, r.pos), nlen = get16(data, r.pos + 2);
            r.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || r.pos + len > data.size()) return formatError("deflate");
            out.append(data, r.pos, len);
            r.pos += len;
        } else if (type == 1) {  // fixed codes (§3.2.6)
            static const auto fixed = [] {
                std::pair<Huffman, Huffman> h;
                int l[288];
                for (int s = 0; s < 288; ++s) l[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
                h.first.build(l, 288);
                int d[30];
                std::fill(d, d + 30, 5);
                h.second.build(d, 30);
                return h;
            }();
            if (!inflateBlock(r, out, fixed.first, fixed.second)) return formatError("deflate");
        } else if (type == 2) {  // dynamic codes (§3.2.7)
            const int hlit = int(r.bits(5)) + 257, hdist = int(r.bits(5)) + 1, hclen = int(r.bits(4)) + 4;
            static constexpr int ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int cl[19] = {};
            for (int k = 0; k < hclen; ++k) cl[ORDER[k]] = int(r.bits(3));
            Huffman clh;
            if (!clh.build(cl, 19) || hlit > 286 || hdist > 30) return formatError("deflate");
            int lens[320] = {};
            int n = 0;
            while (n < hlit + hdist) {
                const int s = clh.decode(r);
                if (s < 0 || r.overrun) return formatError("deflate");
                if (s < 16) {
                    lens[n++] = s;
                    continue;
                }
                int rep = 0, val = 0;
                if (s == 16) {
                    if (n == 0) return formatError("deflate");
                    val = lens[n - 1];
                    rep = 3 + int(r.bits(2));
                } else if (s == 17) {
                    rep = 3 + int(r.bits(3));
                } else {
                    rep = 11 + int(r.bits(7));
                }
                if (n + rep > hlit + hdist) return formatError("deflate");
                while (rep--) lens[n++] = val;
            }
            Huffman lit, dist;
            if (!lit.build(lens, hlit) || !dist.build(lens + hlit, hdist)) return formatError("deflate");
            if (!inflateBlock(r, out, lit, dist)) return formatError("deflate");
        } else {
            return formatError("deflate");
        }
    }
    return out;
}

Result<std::vector<std::pair<std::string, std::string>>> unzip(const std::string& b) {
    // End of central directory record — searched from the end of the file (comment up to 65535 bytes).
    if (b.size() < 22) return formatError("zip");
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i <= 22 + 65535;)
        if (get32(b, i) == 0x06054B50u) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return formatError("zip");
    const uint32_t entries = get16(b, eocd + 10), cdOffset = get32(b, eocd + 16);
    if (entries == 0xFFFF || cdOffset == 0xFFFFFFFFu) return formatError("zip64");
    std::vector<std::pair<std::string, std::string>> out;
    size_t p = cdOffset;
    for (uint32_t e = 0; e < entries; ++e) {
        if (p + 46 > b.size() || get32(b, p) != 0x02014B50u) return formatError("zip");
        const uint32_t method = get16(b, p + 10), crc = get32(b, p + 16), csize = get32(b, p + 20), usize = get32(b, p + 24);
        const uint32_t nlen = get16(b, p + 28), xlen = get16(b, p + 30), clen = get16(b, p + 32), local = get32(b, p + 42);
        if (p + 46 + nlen > b.size()) return formatError("zip");
        const std::string name = b.substr(p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (size_t(local) + 30 > b.size() || get32(b, local) != 0x04034B50u) return formatError("zip");
        const size_t data = size_t(local) + 30 + get16(b, local + 26) + get16(b, local + 28);
        if (data + csize > b.size()) return formatError("zip");
        std::string content;
        if (method == 0) {
            content = b.substr(data, csize);
        } else if (method == 8) {
            auto r = inflateRaw(b.substr(data, csize));
            if (!r) return r.error();
            content = std::move(*r);
        } else {
            return formatError("zip method " + std::to_string(method));
        }
        if (content.size() != usize || crc32(content) != crc) return formatError("zip crc");
        out.emplace_back(name, std::move(content));
    }
    return out;
}

namespace {

// ------------------------------------------------------------ 3MF XML (parsing sufficient for the 3MF core)

struct XmlTag {
    std::string name;  // local name (without the namespace prefix)
    std::map<std::string, std::string> attrs;
    bool closing = false, selfClosing = false;
};

std::string localName(const std::string& n) {
    const auto c = n.find(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

std::string decodeEntities(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos) {
            out.push_back(s[i]);
            continue;
        }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') {
            unsigned long cp = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtoul(e.c_str() + 2, nullptr, 16)
                                                                             : std::strtoul(e.c_str() + 1, nullptr, 10);
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
            else if (cp < 0x10000) out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
            else out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

// Walking document tags: callback on each opening/closing tag; text, comments, PI, CDATA are skipped.
bool forEachTag(const std::string& x, const std::function<void(const XmlTag&)>& f) {
    size_t i = 0;
    while ((i = x.find('<', i)) != std::string::npos) {
        if (x.compare(i, 4, "<!--") == 0) {
            const auto e = x.find("-->", i);
            if (e == std::string::npos) return false;
            i = e + 3;
            continue;
        }
        if (x.compare(i, 9, "<![CDATA[") == 0) {
            const auto e = x.find("]]>", i);
            if (e == std::string::npos) return false;
            i = e + 3;
            continue;
        }
        if (x.compare(i, 2, "<?") == 0 || x.compare(i, 2, "<!") == 0) {
            const auto e = x.find('>', i);
            if (e == std::string::npos) return false;
            i = e + 1;
            continue;
        }
        XmlTag t;
        size_t j = i + 1;
        if (j < x.size() && x[j] == '/') {
            t.closing = true;
            ++j;
        }
        const size_t ns = j;
        while (j < x.size() && !isSpace(x[j]) && x[j] != '>' && x[j] != '/') ++j;
        t.name = localName(x.substr(ns, j - ns));
        while (true) {
            while (j < x.size() && isSpace(x[j])) ++j;
            if (j >= x.size()) return false;
            if (x[j] == '>') {
                ++j;
                break;
            }
            if (x[j] == '/') {
                t.selfClosing = true;
                ++j;
                continue;
            }
            const size_t as = j;
            while (j < x.size() && x[j] != '=' && !isSpace(x[j]) && x[j] != '>') ++j;
            const std::string an = localName(x.substr(as, j - as));
            while (j < x.size() && isSpace(x[j])) ++j;
            if (j >= x.size() || x[j] != '=') return false;
            ++j;
            while (j < x.size() && isSpace(x[j])) ++j;
            if (j >= x.size() || (x[j] != '"' && x[j] != '\'')) return false;
            const char q = x[j++];
            const auto e = x.find(q, j);
            if (e == std::string::npos) return false;
            t.attrs[an] = decodeEntities(x.substr(j, e - j));
            j = e + 1;
        }
        f(t);
        i = j;
    }
    return true;
}

// 3MF matrix "m00 … m32" (point is a row, io.md §4а) → p' = R·p + t.
struct Affine {
    Mat3 R = Mat3::Identity();
    Vec3 t = Vec3::Zero();
    Affine then(const Affine& o) const { return {o.R * R, o.R * t + o.t}; }  // first this, then o
};

std::optional<Affine> parseTransform(const std::string& s) {
    std::istringstream ss(s);
    ss.imbue(std::locale::classic());
    double m[12];
    for (double& v : m)
        if (!(ss >> v)) return std::nullopt;
    Affine a;
    for (int j = 0; j < 3; ++j) {
        a.R(j, 0) = m[0 + j];
        a.R(j, 1) = m[3 + j];
        a.R(j, 2) = m[6 + j];
        a.t[j] = m[9 + j];
    }
    return a;
}

double unitScale(const std::string& u) {
    if (u.empty() || u == "millimeter") return 1;
    if (u == "micron") return 1e-3;
    if (u == "centimeter") return 10;
    if (u == "inch") return 25.4;
    if (u == "foot") return 304.8;
    if (u == "meter") return 1000;
    return 0;
}

struct Object3mf {
    std::string name;
    std::vector<Point3> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<std::pair<std::string, Affine>> components;
};

Result<std::vector<ImportedMesh>> parse3mf(const std::string& bytes) {
    auto parts = unzip(bytes);
    if (!parts) return parts.error();
    auto find = [&](const std::string& n) -> const std::string* {
        std::string key = n;
        if (!key.empty() && key[0] == '/') key.erase(0, 1);
        for (const auto& [name, data] : *parts) {
            std::string a = name, b = key;
            std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (a == b) return &data;
        }
        return nullptr;
    };
    // Main model — the target of the "3dmodel" relationship in _rels/.rels; without it — the first "*.model" part.
    const std::string* model = nullptr;
    if (const std::string* rels = find("_rels/.rels"))
        forEachTag(*rels, [&](const XmlTag& t) {
            if (model || t.closing || t.name != "Relationship") return;
            auto ty = t.attrs.find("Type"), tg = t.attrs.find("Target");
            if (ty != t.attrs.end() && tg != t.attrs.end() && ty->second.size() >= 7 &&
                ty->second.compare(ty->second.size() - 7, 7, "3dmodel") == 0)
                model = find(tg->second);
        });
    if (!model)
        for (const auto& [name, data] : *parts)
            if (name.size() > 6 && name.compare(name.size() - 6, 6, ".model") == 0) {
                model = &data;
                break;
            }
    if (!model) return formatError("3mf model");
    std::map<std::string, Object3mf> objects;
    std::vector<std::pair<std::string, Affine>> items;
    double scale = 1;
    Object3mf* cur = nullptr;
    std::string curId;
    bool bad = false;
    auto num = [&](const XmlTag& t, const char* a, double& v) {
        auto it = t.attrs.find(a);
        if (it == t.attrs.end()) return false;
        std::istringstream ss(it->second);
        ss.imbue(std::locale::classic());
        ss >> v;
        return !ss.fail() && std::isfinite(v);
    };
    const bool parsed = forEachTag(*model, [&](const XmlTag& t) {
        if (bad) return;
        if (t.name == "model" && !t.closing) {
            auto u = t.attrs.find("unit");
            scale = unitScale(u == t.attrs.end() ? std::string() : u->second);
            if (scale == 0) bad = true;
        } else if (t.name == "object") {
            if (t.closing) {
                cur = nullptr;
                return;
            }
            curId = t.attrs.count("id") ? t.attrs.at("id") : std::string();
            cur = &objects[curId];
            if (t.attrs.count("name")) cur->name = t.attrs.at("name");
            if (t.selfClosing) cur = nullptr;
        } else if (t.name == "vertex" && cur && !t.closing) {
            double x = 0, y = 0, z = 0;  // a bad file is rejected, but nothing is read uninitialized (clang)
            if (!num(t, "x", x) || !num(t, "y", y) || !num(t, "z", z)) bad = true;
            cur->vertices.push_back({x, y, z});
        } else if (t.name == "triangle" && cur && !t.closing) {
            double a = 0, b = 0, c = 0;
            if (!num(t, "v1", a) || !num(t, "v2", b) || !num(t, "v3", c) || a < 0 || b < 0 || c < 0) bad = true;
            cur->triangles.push_back({uint32_t(a), uint32_t(b), uint32_t(c)});
        } else if (t.name == "component" && cur && !t.closing) {
            Affine a;
            if (t.attrs.count("transform")) {
                auto tr = parseTransform(t.attrs.at("transform"));
                if (!tr) bad = true;
                else a = *tr;
            }
            cur->components.push_back({t.attrs.count("objectid") ? t.attrs.at("objectid") : std::string(), a});
        } else if (t.name == "item" && !t.closing) {
            Affine a;
            if (t.attrs.count("transform")) {
                auto tr = parseTransform(t.attrs.at("transform"));
                if (!tr) bad = true;
                else a = *tr;
            }
            items.push_back({t.attrs.count("objectid") ? t.attrs.at("objectid") : std::string(), a});
        }
    });
    if (!parsed || bad) return formatError("3mf xml");
    std::vector<ImportedMesh> out;
    // A build item is one mesh: triangles of the object and its components (recursively, depth limited).
    std::function<bool(const std::string&, const Affine&, Welder&, int)> emit = [&](const std::string& id, const Affine& a, Welder& w,
                                                                                  int depth) {
        auto it = objects.find(id);
        if (it == objects.end() || depth > 32) return false;
        const Object3mf& o = it->second;
        std::vector<uint32_t> map(o.vertices.size());
        for (size_t v = 0; v < o.vertices.size(); ++v) map[v] = w.add(scale * (a.R * o.vertices[v] + a.t));
        for (const auto& t : o.triangles) {
            if (t[0] >= map.size() || t[1] >= map.size() || t[2] >= map.size()) return false;
            w.tri(map[t[0]], map[t[1]], map[t[2]]);
        }
        for (const auto& [cid, ca] : o.components)
            if (!emit(cid, ca.then(a), w, depth + 1)) return false;
        return true;
    };
    for (const auto& [id, a] : items) {
        Welder w;
        if (!emit(id, a, w, 0)) return formatError("3mf object");
        w.mesh.name = objects[id].name;
        if (!w.mesh.triangles.empty()) out.push_back(std::move(w.mesh));
    }
    if (out.empty()) return formatError("3mf empty");
    return out;
}

}  // namespace

Result<MeshFileKind> detectMeshFile(const std::string& b) {
    if (b.size() >= 4 && get32(b, 0) == 0x04034B50u) return MeshFileKind::ThreeMf;
    if (b.size() >= 84 && 84 + 50 * uint64_t(get32(b, 80)) == b.size()) return MeshFileKind::StlBinary;
    size_t i = 0;
    while (i < b.size() && isSpace(b[i])) ++i;
    if (b.compare(i, 5, "solid") == 0) return MeshFileKind::StlAscii;
    return formatError("unknown");
}

Result<std::vector<ImportedMesh>> parseMeshFile(const std::string& bytes) {
    auto kind = detectMeshFile(bytes);
    if (!kind) return kind.error();
    switch (*kind) {
        case MeshFileKind::StlBinary: return parseBinaryStl(bytes);
        case MeshFileKind::StlAscii: return parseAsciiStl(bytes);
        case MeshFileKind::ThreeMf: return parse3mf(bytes);
    }
    return formatError("unknown");
}

Result<std::vector<ImportedMesh>> readMeshFile(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return Error("IO_READ").with("file", path).with("detail", std::string("open"));
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseMeshFile(ss.str());
}

}  // namespace construe::io
