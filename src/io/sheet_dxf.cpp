// Export of a drawing sheet to DXF AC1015 (spec/io.md §2г; S3-IO-003): layers by role, GOST 2.303 linetypes,
// lineweights via code 370, texts as contour fills (HATCH) plus an editable copy (TEXT) on a layer that is off;
// handles in order of appearance in the file, deterministic output.
#include "drawing/render.h"
#include "io/dxf_writer.h"
#include "io/sheet_export.h"
#include "base/constants.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace construe::io {

namespace {

using drawing::LineType;
using drawing::Primitive;
using drawing::PrimitiveKind;

constexpr double FULL_TURN_TOL = 1e-12;  // an arc with θ ≥ 2π − tolerance is a circle (§2г)
constexpr const char* EDIT_LAYER = "TEXT-EDITABLE";

// Layer table by role (§2г): order is the order of layers in the LAYER table.
constexpr std::array<const char*, 24> LAYER_ORDER = {"TRIM",   "FRAME",  "TITLEBLOCK", "VISIBLE",  "HIDDEN",     "TANGENT",  "CENTER",
                                                     "THREAD", "HATCH",  "BOUNDARY",   "DESIGNATION", "DIM",     "PMI",
                                                     "CUT",    "ENGRAVE", "LABEL",    "TEXT",
                                                     "THICK",  "THIN",   "DASHED",     "DASHDOT",  "DASHDOTDOT", "WAVY",     EDIT_LAYER};
// Layer colors (ACI) of a cut plan (§2г, drawing.md §2.7): engraving red, labels blue; the others — 7.
int layerColor(std::string_view layer) { return layer == "ENGRAVE" ? 1 : layer == "LABEL" ? 5 : 7; }

// GOST 2.303 linetypes (§2г), except Continuous; elements follow dashPattern (dash > 0, gap < 0).
struct GostLtype {
    LineType type;
    const char* name;
    const char* description;
};
constexpr std::array<GostLtype, 4> GOST_LTYPES = {{{LineType::Dashed, "GOST_DASHED", "GOST 2.303 dashed"},
                                                   {LineType::DashDot, "GOST_DASHDOT", "GOST 2.303 dash-dot"},
                                                   {LineType::DashDotDot, "GOST_DASHDOTDOT", "GOST 2.303 dash-dot-dot"},
                                                   {LineType::Wavy, "GOST_WAVY", "GOST 2.303 wavy (solid)"}}};

const char* ltypeName(LineType t) {
    for (const auto& g : GOST_LTYPES)
        if (g.type == t) return g.name;
    return "Continuous";
}

// Primitive layer by role (§2г "Layers by role").
std::string layerOf(const Primitive& p) {
    const std::string& r = p.role;
    if (r == "trim") return "TRIM";
    if (r == "frame") return "FRAME";
    if (r == "titleBlock") return "TITLEBLOCK";
    if (r == "dim" || r.starts_with("dim:")) return "DIM";
    if (r.starts_with("pmi:")) return "PMI";  // pmi:rest:<el> (drawing.md §6.18а)
    if (r.starts_with("view:")) {
        if (r.find(":pmi:", 5) != std::string::npos) return "PMI";  // view:<id>:pmi:<…> (drawing.md §6.18)
        const std::string cls = r.substr(r.rfind(':') + 1);
        if (cls == "visible") return "VISIBLE";
        if (cls == "hidden") return "HIDDEN";
        if (cls == "tangent") return "TANGENT";
        if (cls == "center") return "CENTER";
        if (cls == "thread") return "THREAD";
        if (cls == "hatch") return "HATCH";
        if (cls == "boundary") return "BOUNDARY";
        if (cls == "label" || cls == "sectionLine" || cls == "detailMark") return "DESIGNATION";
        if (cls == "dim") return "DIM";
        if (cls == "cut") return "CUT";  // cut plan (drawing.md §2.7)
        if (cls == "engrave") return "ENGRAVE";
        if (cls == "cutLabel") return "LABEL";
    }
    if (p.kind == PrimitiveKind::Text) return p.color.empty() ? "TEXT" : "LABEL";  // a cut plan text (FX11-CUT-005)
    switch (p.lineType) {
        case LineType::Dashed: return "DASHED";
        case LineType::DashDot: return "DASHDOT";
        case LineType::DashDotDot: return "DASHDOTDOT";
        case LineType::Wavy: return "WAVY";
        default: return p.weight == drawing::Weight::Thick ? "THICK" : "THIN";
    }
}

// UTF-8 decoding: an invalid sequence gives U+FFFD.
std::vector<char32_t> codePoints(std::string_view s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + size_t(n) >= s.size()) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + size_t(k)]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += size_t(n) + 1;
    }
    return out;
}

// TEXT value string (§2г "Text strings").
std::string dxfText(const std::string& utf8) {
    std::string out;
    for (const char32_t c : codePoints(utf8)) {
        if (c < 0x20) out.push_back(' ');
        else if (c == '%') out += "%%%";
        else if (c < 0x7F) out.push_back(char(c));
        else if (c <= 0xFFFF) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\U+%04X", unsigned(c));
            out += buf;
        } else out.push_back('?');
    }
    return out;
}

// Text font file name and style name (§2г, STYLE).
std::string fontFile(const Primitive& t) {
    std::string f = t.font.empty() ? std::string(drawing::DRAWING_DEFAULT_FONT) : t.font;
    for (char& c : f)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '_';
    return f;
}
std::string styleName(const Primitive& t) {
    std::string f = t.font.empty() ? std::string(drawing::DRAWING_DEFAULT_FONT) : t.font;
    if (const auto dot = f.rfind('.'); dot != std::string::npos) f.resize(dot);
    for (char& c : f)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
    return f;
}

struct Vertex {
    Point2 p;
    double bulge = 0;
};

// Text contour vertices in traversal order (§2г, HATCH).
std::vector<Vertex> contourVertices(const std::vector<font::Curve2>& c) {
    std::vector<Vertex> out;
    if (c.empty()) return out;
    auto bulge = [](const font::Curve2& cv, bool forward) {
        if (cv.kind != font::Curve2::Kind::Arc) return 0.0;
        const double a0 = std::atan2(cv.a.y() - cv.center.y(), cv.a.x() - cv.center.x());
        const double a1 = std::atan2(cv.b.y() - cv.center.y(), cv.b.x() - cv.center.x());
        const double b = std::tan(dxf::sweep(a0, a1) / 4);
        return forward ? b : -b;
    };
    bool forward = true;
    if (c.size() > 1) {
        const auto near = [&](const Point2& p) { return std::min((p - c[1].a).norm(), (p - c[1].b).norm()); };
        forward = near(c[0].b) <= near(c[0].a);
    }
    Point2 cur = forward ? c[0].b : c[0].a;
    out.push_back({forward ? c[0].a : c[0].b, bulge(c[0], forward)});
    for (size_t i = 1; i < c.size(); ++i) {
        const bool f = (c[i].a - cur).norm() <= (c[i].b - cur).norm();
        out.push_back({f ? c[i].a : c[i].b, bulge(c[i], f)});
        cur = f ? c[i].b : c[i].a;
    }
    return out;
}

struct Prepared {
    std::string layer;
    std::optional<drawing::TextOutline> outline;  // text
};

}  // namespace

Result<std::string> sheetDxf(const drawing::SheetGraphics& g) {
    // Preparation: layers, text contours (font errors happen before writing), styles.
    std::vector<Prepared> prep;
    prep.reserve(g.primitives.size());
    std::vector<bool> layerUsed(LAYER_ORDER.size(), false);
    std::vector<std::pair<std::string, std::string>> styles;  // {name, file} in order of first appearance
    auto useLayer = [&](const std::string& name) {
        for (size_t i = 0; i < LAYER_ORDER.size(); ++i)
            if (name == LAYER_ORDER[i]) layerUsed[i] = true;
    };
    for (const auto& p : g.primitives) {
        Prepared pr;
        if (p.kind == PrimitiveKind::Image) {  // shaded image of a view (drawing.md §6.22) — not in DXF (io.md §2г)
            prep.push_back({});
            continue;
        }
        pr.layer = layerOf(p);
        if (p.kind == PrimitiveKind::Text) {
            auto o = drawing::textOutline(p);
            if (!o) return o.error();
            bool any = false;
            for (const auto& c : o->contours) any = any || !c.empty();
            if (!any) {
                prep.push_back({});  // text without contours is not output
                continue;
            }
            pr.outline = std::move(*o);
            useLayer(pr.layer);
            useLayer(EDIT_LAYER);
            const std::string sn = styleName(p);
            bool known = false;
            for (const auto& st : styles) known = known || st.first == sn;
            if (!known) styles.emplace_back(sn, fontFile(p));
        } else if ((p.kind != PrimitiveKind::Line && p.kind != PrimitiveKind::Polyline) || p.points.size() >= 2) {
            useLayer(pr.layer);
        } else {
            prep.push_back({});
            continue;
        }
        prep.push_back(std::move(pr));
    }

    dxf::Tables tables;
    for (const auto& gl : GOST_LTYPES) tables.linetypes.push_back({gl.name, gl.description, drawing::dashPattern(gl.type)});
    for (size_t i = 0; i < LAYER_ORDER.size(); ++i)
        if (layerUsed[i]) {
            const bool edit = std::string_view(LAYER_ORDER[i]) == EDIT_LAYER;
            tables.layers.push_back({LAYER_ORDER[i], layerColor(LAYER_ORDER[i]), "Continuous", edit});
        }
    for (const auto& [name, file] : styles) tables.styles.push_back({name, file});
    dxf::File file(g.width, g.height, tables);
    dxf::Writer& w = file.w();
    auto begin = [&](const char* type, const std::string& layer) { file.entity(type, layer); };
    auto stroke = [&](const Primitive& p) {
        w.pair(6, ltypeName(p.lineType));
        const double mm = p.weight == drawing::Weight::Thick ? drawing::LINE_THICK_MM : drawing::LINE_THIN_MM;
        w.integer(370, std::lround(mm * 100));
    };
    for (size_t i = 0; i < g.primitives.size(); ++i) {
        const Primitive& p = g.primitives[i];
        const Prepared& pr = prep[i];
        if (pr.layer.empty()) continue;
        switch (p.kind) {
            case PrimitiveKind::Line:
                begin("LINE", pr.layer);
                stroke(p);
                w.pair(100, "AcDbLine");
                w.point(10, p.points[0]);
                w.point(11, p.points[1]);
                break;
            case PrimitiveKind::Polyline:
                begin("LWPOLYLINE", pr.layer);
                stroke(p);
                w.pair(100, "AcDbPolyline");
                w.integer(90, int(p.points.size()));
                w.integer(70, p.closed ? 1 : 0);
                for (const auto& q : p.points) w.point(10, q, false);
                break;
            case PrimitiveKind::Circle:
            case PrimitiveKind::Arc: {
                const bool full = p.kind == PrimitiveKind::Circle || dxf::sweep(p.start, p.end) >= TWO_PI - FULL_TURN_TOL;
                begin(full ? "CIRCLE" : "ARC", pr.layer);
                stroke(p);
                w.pair(100, "AcDbCircle");
                w.point(10, p.center);
                w.number(40, p.radius);
                if (!full) {
                    w.pair(100, "AcDbArc");
                    w.pair(50, dxf::degrees(p.start));
                    w.pair(51, dxf::degrees(p.start + dxf::sweep(p.start, p.end)));
                }
                break;
            }
            case PrimitiveKind::Text: {
                int loops = 0;
                for (const auto& c : pr.outline->contours) loops += !c.empty();
                begin("HATCH", pr.layer);
                w.pair(100, "AcDbHatch");
                w.point(10, Point2(0, 0));
                w.number(210, 0), w.number(220, 0), w.number(230, 1);
                w.pair(2, "SOLID");
                w.integer(70, 1);
                w.integer(71, 0);
                w.integer(91, loops);
                for (const auto& c : pr.outline->contours) {
                    if (c.empty()) continue;
                    const auto vs = contourVertices(c);
                    w.integer(92, 2);
                    w.integer(72, 1);
                    w.integer(73, 1);
                    w.integer(93, int(vs.size()));
                    for (const auto& v : vs) {
                        w.point(10, v.p, false);
                        w.number(42, v.bulge);
                    }
                    w.integer(97, 0);
                }
                w.integer(75, 0);
                w.integer(76, 1);
                w.integer(98, 0);
                // Editable copy (§2г, TEXT).
                int h = 0, v = 0;
                for (size_t a = 0, b; a < p.align.size(); a = b + 1) {
                    b = p.align.find(' ', a);
                    if (b == std::string::npos) b = p.align.size();
                    const std::string word = p.align.substr(a, b - a);
                    if (word == "left") h = 0;
                    else if (word == "center") h = 1;
                    else if (word == "right") h = 2;
                    else if (word == "bottom") v = 0;
                    else if (word == "middle") v = 2;
                    else if (word == "top") v = 3;
                }
                begin("TEXT", EDIT_LAYER);
                w.pair(100, "AcDbText");
                w.point(10, p.position);
                w.number(40, p.height);
                w.pair(1, dxfText(p.text));
                w.pair(50, dxf::degrees(p.angle));
                w.pair(7, styleName(p));
                w.integer(72, h);
                w.point(11, p.position);
                w.pair(100, "AcDbText");
                w.integer(73, v);
                break;
            }
        }
    }
    return file.finish();
}

}  // namespace construe::io
