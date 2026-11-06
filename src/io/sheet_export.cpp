// Export of a drawing sheet to PDF and SVG (spec/io.md §2в; S3-IO-001, S3-IO-002): vector, texts as font outlines,
// deterministic output.
#include "io/sheet_export.h"

#include "base/number_format.h"

#include "drawing/render.h"
#include "io/deflate.h"
#include "io/stl.h"
#include "base/geom2.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <tuple>
#include <cstdio>
#include <numbers>
#include <optional>

namespace construe::io {

namespace {

constexpr double MM_TO_PT = 72.0 / 25.4;

double sweepOf(double a0, double a1) { return geom2::ccwSweepPositive(a0, a1); }

double widthMm(drawing::Weight w) { return w == drawing::Weight::Thick ? drawing::LINE_THICK_MM : drawing::LINE_THIN_MM; }

std::string pt(const Point2& p) { return sheetNumber(p.x()) + " " + sheetNumber(p.y()); }

// ---------------------------------------------------------------- PDF

void pdfBeziers(std::string& s, const std::vector<std::array<Point2, 4>>& bz) {
    for (const auto& b : bz) s += pt(b[1]) + " " + pt(b[2]) + " " + pt(b[3]) + " c\n";
}

std::string pdfDash(const std::vector<double>& d) {
    std::string s = "[";
    for (size_t i = 0; i < d.size(); ++i) s += (i ? " " : "") + sheetNumber(d[i]);
    return s + "] 0 d\n";
}

// Color of a primitive (drawing.md §3 color): "#RRGGBB" in upper case, "" — black or not a valid color.
std::string primitiveRgb(const drawing::Primitive& p) {
    const std::string& c = p.color;
    if (c.size() != 7 || c[0] != '#' || !std::all_of(c.begin() + 1, c.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); }))
        return {};
    std::string u = c;
    for (auto& ch : u) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return u == "#000000" ? std::string() : u;
}
// Channels 0…1 of a primitiveRgb value (3 digits after the point); "" — black.
std::tuple<double, double, double> rgbOf(const std::string& c) {
    if (c.empty()) return {0, 0, 0};
    auto ch = [&](size_t i) { return std::round(std::stoi(c.substr(i, 2), nullptr, 16) / 255.0 * 1000) / 1000; };
    return {ch(1), ch(3), ch(5)};
}

// Page content; images of the page in drawing order go to images (named /Im1, /Im2, … on the page,
// §2в «Изображения» ("Images")).
Result<std::string> pdfContent(const drawing::SheetGraphics& g, std::vector<const drawing::RasterImage*>& images) {
    using drawing::PrimitiveKind;
    std::string s = "2.8346457 0 0 2.8346457 0 0 cm\n0 J\n0 j\n0 g\n0 G\n";
    std::optional<double> width;
    std::optional<std::vector<double>> dash;
    std::string color;  // current stroke and fill color, "" — black (the initial 0 g 0 G)
    for (const auto& p : g.primitives) {
        if (p.penOnly) continue;  // the view "as on the drawing" for pen outputs (drawing.md §3, §6.22)
        if (const std::string c = primitiveRgb(p); c != color && p.kind != PrimitiveKind::Image) {  // §2в «Цвет» (FX11-CUT-002)
            const auto [r, gr, b] = rgbOf(c);
            const std::string v = sheetNumber(r) + " " + sheetNumber(gr) + " " + sheetNumber(b);
            s += v + " rg\n" + v + " RG\n";
            color = c;
        }
        if (p.kind == PrimitiveKind::Image) {  // shaded view (drawing.md §6.22): the image in its sheet rectangle
            if (!p.image || p.image->width <= 0 || p.image->height <= 0 || p.points.size() < 2) continue;
            images.push_back(p.image.get());
            const Point2 size = p.points[1] - p.points[0];
            s += "q\n" + sheetNumber(size.x()) + " 0 0 " + sheetNumber(size.y()) + " " + pt(p.points[0]) + " cm\n/Im" +
                 std::to_string(images.size()) + " Do\nQ\n";
            continue;
        }
        if (p.kind == PrimitiveKind::Text) {
            auto o = drawing::textOutline(p);
            if (!o) return o.error();
            if (o->contours.empty()) continue;
            for (const auto& c : o->contours) {
                if (c.empty()) continue;
                const auto oc = drawing::orientContour(c);  // arcs follow the traversal direction (drawing.md §3.1)
                s += pt(oc.front().from) + " m\n";
                for (const auto& cv : oc) {
                    if (!cv.arc) s += pt(cv.to) + " l\n";
                    else pdfBeziers(s, drawing::arcBeziers(cv));
                }
                s += "h\n";
            }
            s += "f\n";
            continue;
        }
        const double w = widthMm(p.weight);
        if (!width || *width != w) {
            s += sheetNumber(w) + " w\n";
            width = w;
        }
        const auto d = drawing::dashPattern(p.lineType);
        if (!dash || *dash != d) {
            s += pdfDash(d);
            dash = d;
        }
        switch (p.kind) {
            case PrimitiveKind::Line:
                if (p.points.size() < 2) continue;
                s += pt(p.points[0]) + " m\n" + pt(p.points[1]) + " l\n";
                break;
            case PrimitiveKind::Polyline:
                if (p.points.empty()) continue;
                s += pt(p.points[0]) + " m\n";
                for (size_t i = 1; i < p.points.size(); ++i) s += pt(p.points[i]) + " l\n";
                if (p.closed && p.filled) {  // dimension arrow: close, fill and stroke with one operator (io.md §2в)
                    s += "b\n";
                    continue;
                }
                if (p.closed) s += "h\n";
                break;
            case PrimitiveKind::Circle: {
                const auto bz = drawing::arcBeziers(p.center, p.radius, 0, TWO_PI);
                s += pt(bz.front()[0]) + " m\n";
                pdfBeziers(s, bz);
                s += "h\n";
                break;
            }
            case PrimitiveKind::Arc: {
                const auto bz = drawing::arcBeziers(p.center, p.radius, p.start, p.end);
                s += pt(bz.front()[0]) + " m\n";
                pdfBeziers(s, bz);
                break;
            }
            default: break;
        }
        s += "S\n";
    }
    return s;
}

// ---------------------------------------------------------------- SVG

std::string xmlEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
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

// SVG arc command to point b; y is already converted to SVG axes.
std::string svgArc(double r, double sweep, const Point2& b, bool ccw = true) {
    // Sweep flag 0: a counter-clockwise arc on the sheet stays counter-clockwise on screen, and in SVG axes (y down)
    // this is a decreasing angle (io.md §2в).
    return "A " + sheetNumber(r) + " " + sheetNumber(r) + " 0 " + (sweep > std::numbers::pi ? "1" : "0") + (ccw ? " 0 " : " 1 ") + pt(b);
}

}  // namespace

std::string sheetNumber(double x) { return fixedTrimmed(x, 4); }

Result<std::string> sheetPdf(const drawing::SheetGraphics& g) { return sheetsPdf({g}); }

// Image XObject stream of one channel set (§2в «Изображения» ("Images")): rows with the PNG «Up» filter,
// zlib-compressed.
std::string pdfImageStream(const drawing::RasterImage& img, bool alpha) {
    const int W = img.width, H = img.height, C = alpha ? 1 : 3;
    std::string raw;
    raw.reserve(size_t(H) * (size_t(W) * size_t(C) + 1));
    for (int y = 0; y < H; ++y) {
        raw.push_back(char(2));  // PNG filter type 2 (Up)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < C; ++c) {
                const size_t ch = alpha ? 3 : size_t(c);
                const uint8_t v = img.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4 + ch];
                const uint8_t up = y > 0 ? img.rgba[(size_t(y - 1) * size_t(W) + size_t(x)) * 4 + ch] : 0;
                raw.push_back(char(uint8_t(v - up)));
            }
    }
    const std::string z = zlibCompress(raw);
    return "/Width " + std::to_string(W) + " /Height " + std::to_string(H) + " /ColorSpace " + (alpha ? "/DeviceGray" : "/DeviceRGB") +
           " /BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 12 /Colors " + std::to_string(C) +
           " /BitsPerComponent 8 /Columns " + std::to_string(W) + " >> /Length " + std::to_string(z.size()) + " >>\nstream\n" + z +
           "\nendstream";
}

// Multi-page PDF (§2в, FX7-DRW-005): page i — objects 3 + 2i (page) and 4 + 2i (content), Producer — 3 + 2n; images
// (§2в «Изображения» ("Images")) — after Producer, two objects each (the image and its alpha mask) in page and drawing
// order. One page gives exactly the single-sheet file.
Result<std::string> sheetsPdf(const std::vector<drawing::SheetGraphics>& pages) {
    std::vector<std::string> contents;
    std::vector<std::vector<const drawing::RasterImage*>> images(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        auto content = pdfContent(pages[i], images[i]);
        if (!content) return content.error();
        contents.push_back(std::move(*content));
    }
    const size_t n = pages.size();
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    auto obj = [&](const std::string& body) {
        offsets.push_back(out.size());
        out += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    obj("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (size_t i = 0; i < n; ++i) kids += (i ? " " : "") + std::to_string(3 + 2 * i) + " 0 R";
    obj("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(n) + " >>");
    size_t nextImage = 4 + 2 * n;  // object number of the first image
    for (size_t i = 0; i < n; ++i) {
        const auto& g = pages[i];
        std::string resources = "<< >>";
        if (!images[i].empty()) {
            resources = "<< /XObject <<";
            for (size_t j = 0; j < images[i].size(); ++j) resources += " /Im" + std::to_string(j + 1) + " " + std::to_string(nextImage + 2 * j) + " 0 R";
            resources += " >> >>";
            nextImage += 2 * images[i].size();
        }
        obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + sheetNumber(g.width * MM_TO_PT) + " " + sheetNumber(g.height * MM_TO_PT) +
            "] /Resources " + resources + " /Contents " + std::to_string(4 + 2 * i) + " 0 R >>");
        obj("<< /Length " + std::to_string(contents[i].size()) + " >>\nstream\n" + contents[i] + "\nendstream");
    }
    obj("<< /Producer (construe) >>");
    const size_t producer = offsets.size();
    for (const auto& page : images)
        for (const auto* img : page) {
            obj("<< /Type /XObject /Subtype /Image /SMask " + std::to_string(offsets.size() + 2) + " 0 R " + pdfImageStream(*img, false));
            obj("<< /Type /XObject /Subtype /Image " + pdfImageStream(*img, true));
        }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R /Info " + std::to_string(producer) +
           " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

Result<std::string> sheetSvg(const drawing::SheetGraphics& g) {
    using drawing::PrimitiveKind;
    const double H = g.height;
    auto P = [&](const Point2& p) { return Point2(p.x(), H - p.y()); };
    auto xy = [&](const char* xn, const char* yn, const Point2& p) {
        const Point2 q = P(p);
        return std::string(" ") + xn + "=\"" + sheetNumber(q.x()) + "\" " + yn + "=\"" + sheetNumber(q.y()) + "\"";
    };
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"" + sheetNumber(g.width) + "mm\" height=\"" + sheetNumber(H) +
         "mm\" viewBox=\"0 0 " + sheetNumber(g.width) + " " + sheetNumber(H) + "\">\n";
    s += "<g fill=\"none\" stroke=\"#000000\" stroke-linecap=\"butt\" stroke-linejoin=\"miter\">\n";
    for (const auto& p : g.primitives) {
        const std::string role = " data-role=\"" + xmlEscape(p.role) + "\"/>\n";
        if (p.kind == PrimitiveKind::Text) {
            auto o = drawing::textOutline(p);
            if (!o) return o.error();
            std::string d;
            for (const auto& c : o->contours) {
                if (c.empty()) continue;
                const auto oc = drawing::orientContour(c);  // arcs follow the traversal direction (drawing.md §3.1)
                d += (d.empty() ? "M " : " M ") + pt(P(oc.front().from));
                for (const auto& cv : oc) {
                    if (!cv.arc) {
                        d += " L " + pt(P(cv.to));
                    } else {
                        const Point2 s0 = cv.ccw ? cv.from : cv.to, s1 = cv.ccw ? cv.to : cv.from;  // counter-clockwise
                        const double r = (s0 - cv.center).norm();
                        const double sw = sweepOf(std::atan2(s0.y() - cv.center.y(), s0.x() - cv.center.x()),
                                                  std::atan2(s1.y() - cv.center.y(), s1.x() - cv.center.x()));
                        d += " " + svgArc(r, sw, P(cv.to), cv.ccw);
                    }
                }
                d += " Z";
            }
            if (d.empty()) continue;
            const std::string tc = primitiveRgb(p);  // color of a cut plan text (FX11-CUT-005)
            s += "<path d=\"" + d + "\" fill=\"" + (tc.empty() ? std::string("#000000") : tc) + "\" stroke=\"none\"" + role;
            continue;
        }
        if (p.kind == PrimitiveKind::Image) continue;  // SVG is a pen output: the view "as on the drawing" (drawing.md §6.22)
        std::string stroke = " stroke-width=\"" + sheetNumber(widthMm(p.weight)) + "\"";
        if (const std::string c = primitiveRgb(p); !c.empty()) stroke += " stroke=\"" + c + "\"";  // §2в «Цвет» (FX11-CUT-002)
        if (const auto dp = drawing::dashPattern(p.lineType); !dp.empty()) {
            stroke += " stroke-dasharray=\"";
            for (size_t i = 0; i < dp.size(); ++i) stroke += (i ? " " : "") + sheetNumber(dp[i]);
            stroke += "\"";
        }
        switch (p.kind) {
            case PrimitiveKind::Line:
                if (p.points.size() < 2) continue;
                s += "<line" + xy("x1", "y1", p.points[0]) + xy("x2", "y2", p.points[1]) + stroke + role;
                break;
            case PrimitiveKind::Polyline: {
                if (p.points.empty()) continue;
                std::string pts;
                for (const auto& q : p.points) {
                    const Point2 v = P(q);
                    pts += (pts.empty() ? "" : " ") + sheetNumber(v.x()) + "," + sheetNumber(v.y());
                }
                s += std::string(p.closed ? "<polygon" : "<polyline") + " points=\"" + pts + "\"" +
                     (p.closed && p.filled ? " fill=\"#000000\"" : "") + stroke + role;
                break;
            }
            case PrimitiveKind::Circle:
                s += "<circle" + xy("cx", "cy", p.center) + " r=\"" + sheetNumber(p.radius) + "\"" + stroke + role;
                break;
            case PrimitiveKind::Arc: {
                const double sw = sweepOf(p.start, p.end);
                const Point2 a = p.center + p.radius * Point2(std::cos(p.start), std::sin(p.start));
                const Point2 b = p.center + p.radius * Point2(std::cos(p.start + sw), std::sin(p.start + sw));
                std::string d = "M " + pt(P(a)) + " ";
                if (sw >= TWO_PI - 1e-12) {  // full turn — as two arcs of π each
                    const Point2 m = p.center + p.radius * Point2(std::cos(p.start + sw / 2), std::sin(p.start + sw / 2));
                    d += svgArc(p.radius, sw / 2, P(m)) + " " + svgArc(p.radius, sw / 2, P(b));
                } else {
                    d += svgArc(p.radius, sw, P(b));
                }
                s += "<path d=\"" + d + "\"" + stroke + role;
                break;
            }
            default: break;
        }
    }
    s += "</g>\n</svg>\n";
    return s;
}

Status writeSheetsPdf(const std::string& path, const std::vector<drawing::SheetGraphics>& pages) {
    auto bytes = sheetsPdf(pages);
    if (!bytes) return bytes.error();
    return writeFileAtomic(path, *bytes);
}

std::string sheetFilePath(const std::string& path, size_t i) {
    if (i == 0) return path;
    const size_t slash = path.find_last_of("/\\");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot < start || dot == start) dot = path.size();
    return path.substr(0, dot) + "-" + std::to_string(i + 1) + path.substr(dot);
}

Status writeSheetsDxf(const std::string& path, const std::vector<drawing::SheetGraphics>& sheets) {
    std::vector<std::string> files;
    for (const auto& g : sheets) {
        auto bytes = sheetDxf(g);
        if (!bytes) return bytes.error();
        files.push_back(std::move(*bytes));
    }
    for (size_t i = 0; i < files.size(); ++i)
        if (auto s = writeFileAtomic(sheetFilePath(path, i), files[i]); !s) return s;
    return okStatus();
}

Status writeSheetFile(const std::string& path, const drawing::SheetGraphics& g, SheetFileFormat format) {
    auto bytes = format == SheetFileFormat::Pdf ? sheetPdf(g) : format == SheetFileFormat::Svg ? sheetSvg(g) : sheetDxf(g);
    if (!bytes) return bytes.error();
    return writeFileAtomic(path, *bytes);
}

}  // namespace construe::io
