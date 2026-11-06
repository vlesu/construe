// TrueType fonts and text outlines (font/font.h, spec/font.md).
#include "font/font.h"

#include "base/program_dir.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace construe::font {

class Font {
public:
    std::string name;
    std::vector<uint8_t> data;
    struct Table {
        uint32_t off = 0, len = 0;
    };
    std::map<std::string, Table> tables;
    int unitsPerEm = 1000;
    int locFormat = 0;
    int numGlyphs = 0;
    int numHMetrics = 0;
    double capHeight = 700;
    uint32_t cmapOff = 0;  // absolute offset of the selected subtable
    int cmapFormat = 0;

    bool in(uint32_t off, uint32_t len) const { return off <= data.size() && len <= data.size() - off; }
    uint16_t u16(uint32_t off) const { return in(off, 2) ? uint16_t(data[off] << 8 | data[off + 1]) : 0; }
    int16_t s16(uint32_t off) const { return int16_t(u16(off)); }
    uint32_t u32(uint32_t off) const { return in(off, 4) ? uint32_t(u16(off)) << 16 | u16(off + 2) : 0; }
    const Table* table(const char* tag) const {
        auto it = tables.find(tag);
        return it != tables.end() ? &it->second : nullptr;
    }
};

namespace {

Error unsupported(const std::string& font, const std::string& detail) {
    return Error("FONT_UNSUPPORTED").with("font", font).with("detail", detail);
}

std::mutex& libMutex() {
    static std::mutex m;
    return m;
}

std::vector<std::string> builtinDirs() {
    std::vector<std::string> d;
    auto addList = [&d](const std::string& s) {  // list of directories separated by ';'
        size_t p = 0;
        while (p <= s.size()) {
            const size_t q = s.find(';', p);
            const std::string part = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (!part.empty()) d.push_back(part);
            if (q == std::string::npos) break;
            p = q + 1;
        }
    };
    if (const char* env = std::getenv("CONSTRUE_FONTS_DIR")) addList(env);
    // Next to the executable for an installed program, otherwise the repository directories (spec/base.md §7a).
    [[maybe_unused]] const std::string exeDir = executableDirectory();
#ifdef CONSTRUE_FONTS_DIR_DEFAULT
    addList(assetDirectory(exeDir, "assets/trafonts", CONSTRUE_FONTS_DIR_DEFAULT));
#endif
#ifdef CONSTRUE_FONTS_DIR_DEFAULT2
    addList(assetDirectory(exeDir, "assets/fonts", CONSTRUE_FONTS_DIR_DEFAULT2));
#endif
    return d;
}

std::vector<std::string>& dirsRef() {
    static std::vector<std::string> dirs = builtinDirs();
    return dirs;
}

std::vector<std::string>& licensedDirsRef() {
    static std::vector<std::string> dirs = builtinDirs();
    return dirs;
}

std::string& preferredRef() {
    static std::string p;
    return p;
}

std::map<std::string, FontPtr>& cacheRef() {
    static std::map<std::string, FontPtr> c;
    return c;
}

// Path → UTF-8 (std::filesystem::path::u8string in C++20 is std::u8string).
std::string u8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lowerAscii(std::string s) {
    for (auto& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    return s;
}

// Glyph of a character (§3.1); 0 — none.
int glyphOf(const Font& f, uint32_t c) {
    const uint32_t o = f.cmapOff;
    if (f.cmapFormat == 4) {
        if (c > 0xFFFF) return 0;
        const int segX2 = f.u16(o + 6);
        const uint32_t ends = o + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
        for (int i = 0; i < segX2 / 2; ++i) {
            const uint16_t end = f.u16(ends + 2 * i);
            if (c > end) continue;
            const uint16_t start = f.u16(starts + 2 * i);
            if (c < start) return 0;
            const uint16_t delta = f.u16(deltas + 2 * i), ro = f.u16(ranges + 2 * i);
            if (ro == 0) return int((c + delta) & 0xFFFF);
            const uint32_t addr = ranges + 2 * i + ro + 2 * (c - start);
            const uint16_t g = f.u16(addr);
            return g ? int((g + delta) & 0xFFFF) : 0;
        }
        return 0;
    }
    if (f.cmapFormat == 12) {
        const uint32_t n = f.u32(o + 12);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t g = o + 16 + 12 * i, s = f.u32(g), e = f.u32(g + 4);
            if (c >= s && c <= e) return int(f.u32(g + 8) + (c - s));
        }
    }
    return 0;
}

int advanceOf(const Font& f, int g) {
    const auto* t = f.table("hmtx");
    const int i = std::min(g, f.numHMetrics - 1);
    return t ? f.u16(t->off + 4 * uint32_t(std::max(i, 0))) : 0;
}

// Glyph outline point in font units.
struct GPoint {
    double x = 0, y = 0;
    bool on = true;
};
using GContour = std::vector<GPoint>;

// Glyph outlines (§3.2); false — glyph not supported (point matching, corruption, depth).
bool glyphContours(const Font& f, int g, std::vector<GContour>& out, int depth) {
    if (depth > 8 || g < 0 || g >= f.numGlyphs) return false;
    const auto* loca = f.table("loca");
    const auto* glyf = f.table("glyf");
    uint32_t a, b;
    if (f.locFormat == 0) a = 2u * f.u16(loca->off + 2 * g), b = 2u * f.u16(loca->off + 2 * (g + 1));
    else a = f.u32(loca->off + 4 * g), b = f.u32(loca->off + 4 * (g + 1));
    if (b <= a) return true;  // glyph without an outline (space)
    const uint32_t p = glyf->off + a;
    if (!f.in(p, b - a)) return false;
    const int nc = f.s16(p);
    if (nc >= 0) {
        std::vector<int> endPts(static_cast<size_t>(nc));
        for (int i = 0; i < nc; ++i) endPts[size_t(i)] = f.u16(p + 10 + 2 * i);
        const int nPts = nc ? endPts.back() + 1 : 0;
        uint32_t q = p + 10 + 2 * nc;
        q += 2 + f.u16(q);  // instructions
        std::vector<uint8_t> flags;
        while (int(flags.size()) < nPts) {
            if (!f.in(q, 1)) return false;
            const uint8_t fl = f.data[q++];
            flags.push_back(fl);
            if (fl & 8) {
                if (!f.in(q, 1)) return false;
                for (int r = f.data[q++]; r > 0 && int(flags.size()) < nPts; --r) flags.push_back(fl);
            }
        }
        std::vector<GPoint> pts(static_cast<size_t>(nPts));
        int v = 0;
        for (int i = 0; i < nPts; ++i) {
            const uint8_t fl = flags[size_t(i)];
            if (fl & 2) {
                if (!f.in(q, 1)) return false;
                const int d = f.data[q++];
                v += (fl & 16) ? d : -d;
            } else if (!(fl & 16)) {
                v += f.s16(q);
                q += 2;
            }
            pts[size_t(i)].x = v;
            pts[size_t(i)].on = fl & 1;
        }
        v = 0;
        for (int i = 0; i < nPts; ++i) {
            const uint8_t fl = flags[size_t(i)];
            if (fl & 4) {
                if (!f.in(q, 1)) return false;
                const int d = f.data[q++];
                v += (fl & 32) ? d : -d;
            } else if (!(fl & 32)) {
                v += f.s16(q);
                q += 2;
            }
            pts[size_t(i)].y = v;
        }
        int s = 0;
        for (int e : endPts) {
            if (e < s || e >= nPts) return false;
            out.emplace_back(pts.begin() + s, pts.begin() + e + 1);
            s = e + 1;
        }
        return true;
    }
    // Composite glyph.
    uint32_t q = p + 10;
    for (;;) {
        const uint16_t fl = f.u16(q), gi = f.u16(q + 2);
        q += 4;
        double dx, dy;
        if (fl & 1) dx = f.s16(q), dy = f.s16(q + 2), q += 4;
        else dx = int8_t(f.data[q]), dy = int8_t(f.data[q + 1]), q += 2;
        if (!(fl & 2)) return false;  // point matching — not supported
        double m[4] = {1, 0, 0, 1};
        auto f2 = [&](uint32_t o) { return f.s16(o) / 16384.0; };
        if (fl & 8) m[0] = m[3] = f2(q), q += 2;
        else if (fl & 64) m[0] = f2(q), m[3] = f2(q + 2), q += 4;
        else if (fl & 128) m[0] = f2(q), m[1] = f2(q + 2), m[2] = f2(q + 4), m[3] = f2(q + 6), q += 8;
        std::vector<GContour> sub;
        if (!glyphContours(f, gi, sub, depth + 1)) return false;
        for (auto& c : sub) {
            for (auto& pt : c) {
                const double x = pt.x, y = pt.y;
                pt.x = m[0] * x + m[2] * y + dx;
                pt.y = m[1] * x + m[3] * y + dy;
            }
            out.push_back(std::move(c));
        }
        if (!(fl & 32)) break;
    }
    return true;
}

// Outline segment: line segment (p0, p2) or quadratic curve (p0, p1, p2).
struct QSeg {
    bool quad = false;
    Point2 p0, p1, p2;
};

std::vector<QSeg> toSegments(const GContour& c) {
    std::vector<QSeg> out;
    const size_t n = c.size();
    if (n < 2) return out;
    auto P = [&](size_t i) { return Point2(c[i % n].x, c[i % n].y); };
    // Start — the first on-curve point; if all are off-curve — the midpoint of the first two.
    size_t first = n;
    for (size_t i = 0; i < n; ++i)
        if (c[i].on) {
            first = i;
            break;
        }
    Point2 start;
    size_t i0;
    if (first < n) start = P(first), i0 = first + 1;
    else start = (P(0) + P(1)) / 2, i0 = 1;
    Point2 cur = start;
    std::optional<Point2> ctrl;
    for (size_t k = 0; k < n; ++k) {
        const size_t i = i0 + k;
        const Point2 p = P(i);
        const bool on = c[i % n].on;
        const bool last = k == n - 1;
        if (on) {
            if (ctrl) out.push_back({true, cur, *ctrl, p}), ctrl.reset();
            else if ((p - cur).norm() > 0) out.push_back({false, cur, cur, p});
            cur = p;
        } else {
            if (ctrl) {
                const Point2 mid = (*ctrl + p) / 2;
                out.push_back({true, cur, *ctrl, mid});
                cur = mid;
            }
            ctrl = p;
        }
        if (last) {
            // Closing back to the start.
            if (ctrl) out.push_back({true, cur, *ctrl, start});
            else if ((start - cur).norm() > 0) out.push_back({false, cur, cur, start});
        }
    }
    // When starting from the midpoint of the first two points, the first (off-curve) point is handled by the loop.
    return out;
}

Point2 quadAt(const QSeg& s, double t) { return (1 - t) * (1 - t) * s.p0 + 2 * (1 - t) * t * s.p1 + t * t * s.p2; }

double chordDist(const Point2& p, const Point2& a, const Point2& b) {
    const Point2 d = b - a;
    const double l = d.norm();
    if (l <= 0) return (p - a).norm();
    return std::abs(d.x() * (p - a).y() - d.y() * (p - a).x()) / l;
}

void approx(const QSeg& s, double t0, double t1, const Point2& P0, const Point2& P1, int depth, double tol, std::vector<Curve2>& out) {
    auto line = [&] { out.push_back({Curve2::Kind::Line, P0, P1, Point2::Zero()}); };
    if ((P1 - P0).norm() <= tol || depth >= TEXT_APPROX_DEPTH) return line();
    const double tm = (t0 + t1) / 2;
    const Point2 Pm = quadAt(s, tm);
    const Point2 qa = quadAt(s, (t0 + tm) / 2), qb = quadAt(s, (tm + t1) / 2);
    if (chordDist(Pm, P0, P1) <= tol / 4 && chordDist(qa, P0, P1) <= tol && chordDist(qb, P0, P1) <= tol) return line();
    // Circle through P0, Pm, P1.
    const Point2 a = P0, b = Pm, c = P1;
    const double d = 2 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y()) + c.x() * (a.y() - b.y()));
    if (std::abs(d) > 1e-300) {
        const double a2 = a.squaredNorm(), b2 = b.squaredNorm(), c2 = c.squaredNorm();
        const Point2 ctr((a2 * (b.y() - c.y()) + b2 * (c.y() - a.y()) + c2 * (a.y() - b.y())) / d,
                         (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x()) + c2 * (b.x() - a.x())) / d);
        const double r = (a - ctr).norm();
        if (std::abs((qa - ctr).norm() - r) <= tol && std::abs((qb - ctr).norm() - r) <= tol) {
            const Point2 u = Pm - P0, v = P1 - P0;
            const bool ccw = u.x() * v.y() - u.y() * v.x() > 0;  // P0 → Pm → P1 is a left turn (counter-clockwise)
            if (ccw) out.push_back({Curve2::Kind::Arc, P0, P1, ctr});
            else out.push_back({Curve2::Kind::Arc, P1, P0, ctr});
            return;
        }
    }
    approx(s, t0, tm, P0, Pm, depth + 1, tol, out);
    approx(s, tm, t1, Pm, P1, depth + 1, tol, out);
}

// UTF-8 decoding (an invalid sequence gives the character U+FFFD).
std::vector<uint32_t> utf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + size_t(n) >= s.size() + (n == 0 ? 1 : 0)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = n == 0 ? c : c & (0x3F >> n);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            if (i + size_t(k) >= s.size() || (uint8_t(s[i + size_t(k)]) & 0xC0) != 0x80) ok = false;
            else cp = cp << 6 | (uint8_t(s[i + size_t(k)]) & 0x3F);
        }
        out.push_back(ok ? cp : 0xFFFD);
        i += ok ? size_t(n) + 1 : 1;
    }
    return out;
}

std::string utf8Of(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) s += char(0xC0 | cp >> 6), s += char(0x80 | (cp & 0x3F));
    else if (cp < 0x10000) s += char(0xE0 | cp >> 12), s += char(0x80 | (cp >> 6 & 0x3F)), s += char(0x80 | (cp & 0x3F));
    else s += char(0xF0 | cp >> 18), s += char(0x80 | (cp >> 12 & 0x3F)), s += char(0x80 | (cp >> 6 & 0x3F)), s += char(0x80 | (cp & 0x3F));
    return s;
}

}  // namespace

void setFontDirectories(std::vector<std::string> dirs) {
    std::lock_guard<std::mutex> lk(libMutex());
    dirsRef() = std::move(dirs);
}

std::vector<std::string> fontDirectories() {
    std::lock_guard<std::mutex> lk(libMutex());
    return dirsRef();
}

std::vector<std::string> defaultFontDirectories() { return builtinDirs(); }

void setLicensedFontDirectories(std::vector<std::string> dirs) {
    std::lock_guard<std::mutex> lk(libMutex());
    licensedDirsRef() = std::move(dirs);
}

std::vector<std::string> licensedFontDirectories() {
    std::lock_guard<std::mutex> lk(libMutex());
    return licensedDirsRef();
}

bool isExcludedFont(const std::string& name) {
    for (const auto& x : EXCLUDED_FONTS)
        if (lowerAscii(x) == lowerAscii(name)) return true;
    return false;
}

std::vector<std::string> availableFonts() {
    const auto norm = [](const std::string& d) {  // lexically normalized, '/' separators, no trailing '/'
        const auto g = std::filesystem::u8path(d).lexically_normal().generic_u8string();
        std::string r(g.begin(), g.end());
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        return r;
    };
    std::set<std::string> licensed;
    for (const auto& d : licensedFontDirectories()) licensed.insert(norm(d));
    std::set<std::string> names;
    for (const auto& d : fontDirectories()) {
        std::error_code ec;
        std::vector<std::string> fonts;
        std::set<std::string> files;  // lower-case names of all files: license lookup (§2а)
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(d), ec)) {
            if (!e.is_regular_file()) continue;
            const std::string n = u8(e.path().filename());
            files.insert(lowerAscii(n));
            if (lowerAscii(u8(e.path().extension())) == ".ttf") fonts.push_back(n);
        }
        // A distribution directory lists a font only with its license file next to it (§2а, S7-FONT-001).
        const bool needLicense = licensed.count(norm(d)) > 0;
        for (const auto& n : fonts)
            if (!isExcludedFont(n) && (!needLicense || files.count(lowerAscii(n.substr(0, n.size() - 4)) + ".license.txt")))
                names.insert(n);
    }
    return {names.begin(), names.end()};
}

Result<FontPtr> parseFont(std::vector<uint8_t> bytes, const std::string& name) {
    auto f = std::make_shared<Font>();
    f->name = name;
    f->data = std::move(bytes);
    if (f->data.size() < 12) return unsupported(name, "format");
    const uint32_t sig = f->u32(0);
    if (sig == 0x4F54544F) return unsupported(name, "cff");  // 'OTTO'
    if (sig != 0x00010000 && sig != 0x74727565) return unsupported(name, "format");
    const int n = f->u16(4);
    for (int i = 0; i < n; ++i) {
        const uint32_t r = 12 + 16 * uint32_t(i);
        if (!f->in(r, 16)) return unsupported(name, "format");
        std::string tag(reinterpret_cast<const char*>(&f->data[r]), 4);
        const uint32_t off = f->u32(r + 8), len = f->u32(r + 12);
        if (!f->in(off, len)) return unsupported(name, "format");
        f->tables[tag] = {off, len};
    }
    if (f->table("CFF ") && !f->table("glyf")) return unsupported(name, "cff");
    for (const char* t : {"head", "maxp", "hhea", "hmtx", "loca", "glyf", "cmap"})
        if (!f->table(t)) return unsupported(name, std::string("table ") + t);
    f->unitsPerEm = std::max<int>(1, f->u16(f->table("head")->off + 18));
    f->locFormat = f->s16(f->table("head")->off + 50);
    f->numGlyphs = f->u16(f->table("maxp")->off + 4);
    f->numHMetrics = std::max<int>(1, f->u16(f->table("hhea")->off + 34));
    f->capHeight = 0.7 * f->unitsPerEm;
    if (const auto* os2 = f->table("OS/2"); os2 && os2->len >= 90)
        if (const int ch = f->s16(os2->off + 88); ch > 0) f->capHeight = ch;
    // cmap (§3.1).
    const uint32_t cm = f->table("cmap")->off;
    const int nsub = f->u16(cm + 2);
    const std::pair<int, int> pref[] = {{3, 10}, {0, 4}, {3, 1}, {0, 3}};
    const int prefFmt[] = {12, 12, 4, 4};
    for (int k = 0; k < 4 && !f->cmapOff; ++k)
        for (int i = 0; i < nsub; ++i) {
            const uint32_t r = cm + 4 + 8 * uint32_t(i);
            if (f->u16(r) != pref[k].first || f->u16(r + 2) != pref[k].second) continue;
            const uint32_t o = cm + f->u32(r + 4);
            if (f->u16(o) != prefFmt[k]) continue;
            f->cmapOff = o;
            f->cmapFormat = prefFmt[k];
            break;
        }
    if (!f->cmapOff) return unsupported(name, "table cmap");
    return FontPtr(f);
}

Result<FontPtr> loadFont(const std::string& name) {
    std::filesystem::path found;
    for (const auto& d : fontDirectories()) {
        const auto dir = std::filesystem::u8path(d);
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / std::filesystem::u8path(name), ec)) {
            found = dir / std::filesystem::u8path(name);
            break;
        }
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file() && lowerAscii(u8(e.path().filename())) == lowerAscii(name)) {
                found = e.path();
                break;
            }
        if (!found.empty()) break;
    }
    if (found.empty()) return Error("FONT_NOT_FOUND").with("font", name);
    const std::string key = u8(found);
    {
        std::lock_guard<std::mutex> lk(libMutex());
        if (auto it = cacheRef().find(key); it != cacheRef().end()) return it->second;
    }
    std::ifstream in(found, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto f = parseFont(std::move(bytes), name);
    if (!f) return f.error();
    std::lock_guard<std::mutex> lk(libMutex());
    cacheRef()[key] = *f;
    return *f;
}

namespace {
// Equivalent characters tried when the font has no glyph for the character (§4 step 2, FX7-DRW-002): the diameter
// sign is written as U+00D8, U+2300 or U+2205 — GOST fonts often carry only one of them.
std::vector<uint32_t> glyphSubstitutes(uint32_t cp) {
    switch (cp) {
        case 0x00D8: return {0x2300, 0x2205};
        case 0x2300: return {0x00D8, 0x2205};
        case 0x2205: return {0x2300, 0x00D8};
        default: return {};
    }
}

// Glyph of the character or of its first equivalent with a readable outline (contours filled); 0 — none.
int usableGlyph(const Font& f, uint32_t cp, std::vector<GContour>& contours) {
    std::vector<uint32_t> tries{cp};
    for (uint32_t x : glyphSubstitutes(cp)) tries.push_back(x);
    for (uint32_t c : tries) {
        contours.clear();
        const int g = glyphOf(f, c);
        if (g != 0 && glyphContours(f, g, contours, 0)) return g;
    }
    contours.clear();
    return 0;
}
}  // namespace

namespace {

// Synthesized bold (font.md §4а): every control point of every contour moves outward of the filled area by d (font
// units) — to the intersection of the two neighbouring edges shifted by d along their normals. "Outward" is the left
// normal for a glyph whose total signed area (shoelace over the control points of all its contours) is negative
// (clockwise outer contours, TrueType), otherwise the right one; holes have the opposite direction and shrink.
void embolden(std::vector<GContour>& contours, double d) {
    double area = 0;
    for (const auto& c : contours)
        for (size_t i = 0; i < c.size(); ++i) {
            const GPoint& a = c[i];
            const GPoint& b = c[(i + 1) % c.size()];
            area += a.x * b.y - b.x * a.y;
        }
    const double side = area < 0 ? 1.0 : -1.0;  // +1: left normal (−dy, dx)
    for (auto& c : contours) {
        const size_t n = c.size();
        if (n < 3) continue;
        std::vector<GPoint> out = c;
        auto P = [&](size_t i) { return Point2(c[i % n].x, c[i % n].y); };
        for (size_t i = 0; i < n; ++i) {
            const Point2 p = P(i);
            // Nearest distinct neighbours (coincident control points are skipped).
            Point2 prev = p, next = p;
            for (size_t k = 1; k < n && (prev - p).norm() == 0; ++k) prev = P(i + n - k);
            for (size_t k = 1; k < n && (next - p).norm() == 0; ++k) next = P(i + k);
            const Point2 e1 = p - prev, e2 = next - p;
            if (e1.norm() == 0 || e2.norm() == 0) continue;
            const Point2 u1 = e1.normalized(), u2 = e2.normalized();
            const Point2 n1 = side * Point2(-u1.y(), u1.x()), n2 = side * Point2(-u2.y(), u2.x());
            const double den = 1 + n1.dot(n2);
            Point2 shift = den < 1e-6 ? Point2(d * n1) : Point2(d * (n1 + n2) / den);
            if (shift.norm() > TEXT_BOLD_MAX_MITER * d) shift = shift.normalized() * (TEXT_BOLD_MAX_MITER * d);
            out[i].x += shift.x(), out[i].y += shift.y();
        }
        c = std::move(out);
    }
}

}  // namespace

Layout layoutText(const Font& f, const std::string& text, double height, const Point2& anchor, double angle,
                  const std::vector<StyleSpan>& spans) {
    Layout out;
    const double s = height / f.capHeight;
    const double ca = std::cos(angle), sa = std::sin(angle);
    auto place = [&](const Point2& p) { return Point2(anchor.x() + s * (ca * p.x() - sa * p.y()), anchor.y() + s * (sa * p.x() + ca * p.y())); };
    const double tol = std::max(TEXT_CURVE_TOL_REL * height, TEXT_CURVE_TOL_MIN);
    const double cap = f.capHeight;
    double pen = 0, baseline = 0;
    std::set<uint32_t> warned;
    // Underline of the current line (§4а): pen ranges of underlined characters and the lowest outline point.
    std::vector<std::pair<double, double>> under;
    double lowest = 0;
    auto addContour = [&](const GContour& c) {
        const auto segs = toSegments(c);
        if (segs.size() < 2) return;
        std::vector<Curve2> cur;
        for (const auto& sg : segs) {
            const Point2 a = place(sg.p0), b = place(sg.p2);
            if (!sg.quad) {
                if ((b - a).norm() > 0) cur.push_back({Curve2::Kind::Line, a, b, Point2::Zero()});
                continue;
            }
            QSeg w{true, a, place(sg.p1), b};
            approx(w, 0, 1, a, b, 0, tol, cur);
        }
        // Zero polyline area — the contour is discarded (§3.2).
        double area = 0;
        for (const auto& k : cur) area += k.a.x() * k.b.y() - k.b.x() * k.a.y();
        if (cur.size() >= 2 && std::abs(area) > 0) out.contours.push_back(std::move(cur));
    };
    auto finishLine = [&] {
        const double top = std::min(-TEXT_UNDERLINE_TOP * cap, lowest - TEXT_UNDERLINE_GAP * cap);
        const double bottom = top - TEXT_UNDERLINE_THICKNESS * cap;
        for (const auto& [x0, x1] : under) {
            if (!(x1 > x0)) continue;
            GContour r{{x0, baseline + bottom, true}, {x1, baseline + bottom, true}, {x1, baseline + top, true}, {x0, baseline + top, true}};
            addContour(r);
        }
        under.clear();
        lowest = 0;
    };
    int index = 0;
    for (uint32_t cp : utf8(text)) {
        const int at = index++;
        if (cp == '\n') {  // next line (§4 item 6)
            finishLine();
            pen = 0;
            baseline -= TEXT_LINE_SPACING * cap;
            continue;
        }
        StyleSpan st;
        for (const auto& sp : spans)
            if (at >= sp.from && at < sp.to) st.bold |= sp.bold, st.italic |= sp.italic, st.underline |= sp.underline;
        std::vector<GContour> contours;
        const int g = usableGlyph(f, cp, contours);
        if (g == 0) {
            if (warned.insert(cp).second) out.warnings.push_back(Error("FONT_MISSING_GLYPH").with("char", utf8Of(cp)));
            continue;
        }
        const double grow = st.bold ? TEXT_BOLD_SHIFT * cap : 0;
        if (st.bold) embolden(contours, grow);
        for (auto& c : contours) {
            for (auto& p : c) {
                if (st.italic) p.x += TEXT_ITALIC_SLANT * p.y;
                p.x += pen + grow;
                lowest = std::min(lowest, p.y);
                p.y += baseline;
            }
            addContour(c);
        }
        const double adv = advanceOf(f, g) + 2 * grow;
        if (st.underline) {
            if (!under.empty() && under.back().second == pen) under.back().second = pen + adv;
            else under.emplace_back(pen, pen + adv);
        }
        pen += adv;
    }
    finishLine();
    return out;
}

std::optional<double> advanceWidth(const Font& f, uint32_t codepoint) {
    std::vector<GContour> contours;
    const int g = usableGlyph(f, codepoint, contours);
    if (g == 0) return std::nullopt;
    return double(advanceOf(f, g));
}

int unitsPerEm(const Font& f) { return f.unitsPerEm; }
double capHeight(const Font& f) { return f.capHeight; }

namespace {
// The first name of the wanted list that is among names (ASCII case-insensitive), library spelling; else empty.
std::string firstPresent(const std::vector<std::string>& names, const std::vector<std::string>& wanted) {
    for (const auto& n : wanted)
        for (const auto& x : names)
            if (lowerAscii(x) == lowerAscii(n)) return x;
    return {};
}
}  // namespace

void setPreferredFont(std::string name) {
    std::lock_guard<std::mutex> lk(libMutex());
    preferredRef() = std::move(name);
}

std::string preferredFont() {
    std::lock_guard<std::mutex> lk(libMutex());
    return preferredRef();
}

std::string builtinDefaultFont() {
    const auto names = availableFonts();
    if (auto n = firstPresent(names, TEXT_DEFAULT_FONTS); !n.empty()) return n;
    return names.empty() ? std::string() : names.front();
}

std::string defaultFont() {
    if (const std::string p = preferredFont(); !p.empty())
        if (auto n = firstPresent(availableFonts(), {p}); !n.empty()) return n;
    return builtinDefaultFont();
}

const std::vector<std::string>& drawingDefaultFonts(DrawingStandard s) {
    return s == DrawingStandard::Iso ? DRAWING_DEFAULT_FONTS_ISO : DRAWING_DEFAULT_FONTS_GOST;
}

std::string defaultDrawingFont(DrawingStandard s) {
    const auto names = availableFonts();
    if (auto n = firstPresent(names, drawingDefaultFonts(s)); !n.empty()) return n;
    return names.empty() ? std::string() : names.front();
}

namespace {
void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | c >> 6);
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | c >> 12);
        out += char(0x80 | (c >> 6 & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | c >> 18);
        out += char(0x80 | (c >> 12 & 0x3F));
        out += char(0x80 | (c >> 6 & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}
}  // namespace

// §3.3: name ID 1 record — first Unicode (platform 3, encodings 1/10, or platform 0) in UTF-16BE, otherwise
// Macintosh (platform 1) — ASCII characters only.
std::string familyName(const Font& f) {
    const auto* t = f.table("name");
    if (!t) return {};
    const uint32_t base = t->off;
    const int count = f.u16(base + 2);
    const uint32_t strings = base + f.u16(base + 4);
    std::string mac;
    for (int i = 0; i < count; ++i) {
        const uint32_t r = base + 6 + 12 * uint32_t(i);
        const int platform = f.u16(r), encoding = f.u16(r + 2), nameId = f.u16(r + 6);
        const uint32_t len = f.u16(r + 8), off = strings + f.u16(r + 10);
        if (nameId != 1 || !f.in(off, len)) continue;
        if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0) {
            std::string out;
            for (uint32_t k = 0; k + 1 < len; k += 2) {
                uint32_t c = f.u16(off + k);
                if (c >= 0xD800 && c < 0xDC00 && k + 3 < len) {
                    const uint32_t lo = f.u16(off + k + 2);
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    k += 2;
                }
                appendUtf8(out, c);
            }
            return out;
        }
        if (platform == 1 && mac.empty())
            for (uint32_t k = 0; k < len; ++k)
                if (f.data[off + k] < 0x80) mac += char(f.data[off + k]);
    }
    return mac;
}

}  // namespace construe::font
