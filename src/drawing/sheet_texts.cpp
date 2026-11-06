// Texts on a sheet (spec/drawing.md §6.23, FX7-DRW-006): a block of lines at any place of the sheet; styles of fragments
// (FX9-DRW-006) and a frame with word wrapping (FX9-DRW-007).
#include "drawing/render.h"
#include "drawing/standard.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace construe::drawing {

namespace {

// UTF-8 code points of a string, each as its bytes.
std::vector<std::string> codePoints(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// A line made of code points idx (indices into cps): its text and its styles (indices of the line).
SheetTextLine makeLine(const std::vector<std::string>& cps, const std::vector<int>& idx, size_t from, size_t to,
                       const std::vector<font::StyleSpan>& spans) {
    SheetTextLine l;
    for (size_t k = from; k < to; ++k) {
        l.text += cps[size_t(idx[k])];
        bool b = false, it = false, un = false;
        for (const auto& sp : spans)
            if (idx[k] >= sp.from && idx[k] < sp.to) b |= sp.bold, it |= sp.italic, un |= sp.underline;
        if (!(b || it || un)) continue;
        const int pos = int(k - from);
        if (!l.styles.empty()) {
            auto& last = l.styles.back();
            if (last.to == pos && last.bold == b && last.italic == it && last.underline == un) {
                last.to = pos + 1;
                continue;
            }
        }
        l.styles.push_back({pos, pos + 1, b, it, un});
    }
    return l;
}

}  // namespace

std::vector<std::string> sheetTextLines(const std::string& text) {
    std::vector<std::string> out(1);
    for (char ch : text) {
        if (ch == '\n') out.emplace_back();
        else if (ch != '\r') out.back().push_back(ch);
    }
    return out;
}

std::string sheetTextFont(const doc::SheetText& t) {
    if (t.font == "gost") return drawingFontFor(font::DrawingStandard::Gost);
    if (t.font == "iso") return drawingFontFor(font::DrawingStandard::Iso);
    return t.font;  // a font file of the library (FX10-DRW-004); empty — the drawing font (§3.2)
}

std::vector<SheetTextLine> sheetTextLayoutLines(const doc::SheetText& t) {
    const auto cps = codePoints(t.text);
    const std::string font = sheetTextFont(t);
    // Paragraphs: code point indices between '\n' ('\r' dropped).
    std::vector<std::vector<int>> paras(1);
    for (size_t i = 0; i < cps.size(); ++i) {
        if (cps[i] == "\n") paras.emplace_back();
        else if (cps[i] != "\r") paras.back().push_back(int(i));
    }
    std::vector<SheetTextLine> out;
    for (size_t pi = 0; pi < paras.size(); ++pi) {
        const auto& p = paras[pi];
        const std::string align = pi < t.aligns.size() && (t.aligns[pi] == "center" || t.aligns[pi] == "right") ? t.aligns[pi] : "left";
        const size_t first = out.size();
        if (t.boxWidth <= 0 || p.empty()) {
            out.push_back(makeLine(cps, p, 0, p.size(), t.styles));
            out.back().align = align;
            continue;
        }
        auto fits = [&](size_t from, size_t to) {
            const SheetTextLine l = makeLine(cps, p, from, to, t.styles);
            const auto w = textWidth(l.text, t.height, font, l.styles);
            return !w || *w <= t.boxWidth + 1e-9;  // font not found: no wrapping
        };
        auto space = [&](size_t k) { return cps[size_t(p[k])] == " "; };
        size_t start = 0;
        while (start < p.size()) {
            // Ends of whole words: before each space after a non-space, and the paragraph end.
            size_t best = start, firstEnd = p.size();
            bool found = false;
            for (size_t j = start + 1; j <= p.size(); ++j) {
                if (j < p.size() && !(space(j) && !space(j - 1))) continue;
                if (firstEnd == p.size()) firstEnd = j;
                if (!fits(start, j)) break;
                best = j;
                found = true;
            }
            if (!found) {  // the first word is wider than the frame: by characters, at least one
                best = start + 1;
                while (best < firstEnd && fits(start, best + 1)) ++best;
            }
            out.push_back(makeLine(cps, p, start, best, t.styles));
            start = best;
            while (start < p.size() && space(start)) ++start;  // the spaces at a wrap are not drawn
        }
        for (size_t k = first; k < out.size(); ++k) out[k].align = align;  // wrapped lines keep their paragraph's
    }
    return out;
}

std::optional<std::array<Point2, 4>> sheetTextFrame(const doc::SheetText& t, size_t lineCount) {
    if (t.boxWidth <= 0) return std::nullopt;
    const double n = double(std::max<size_t>(lineCount, 1));
    const double top = SHEET_TEXT_FRAME_TOP * t.height;
    const double h = std::max(t.boxHeight, n * SHEET_TEXT_LINE_STEP * t.height);
    const double a = t.angle * std::numbers::pi / 180, c = std::cos(a), s = std::sin(a);
    const Point2 o(t.position[0], t.position[1]);
    auto at = [&](double x, double y) { return Point2(o.x() + c * x - s * y, o.y() + s * x + c * y); };
    return std::array<Point2, 4>{at(0, top), at(t.boxWidth, top), at(t.boxWidth, top - h), at(0, top - h)};
}

void appendTexts(const doc::Sheet& sheet, SheetGraphics& out) {
    for (const auto& t : sheet.texts) {
        const double a = t.angle * std::numbers::pi / 180;
        const Point2 down(std::sin(a), -std::cos(a));  // the next line — below, across the rotated baseline
        const auto lines = sheetTextLayoutLines(t);
        const std::string font = sheetTextFont(t);
        // Alignment (S7F-DRW-002): a line is shifted along the baseline within the frame width, without a frame — within
        // the widest line; a font not found — no shift.
        std::vector<std::optional<double>> widths;
        double block = t.boxWidth;
        for (const auto& l : lines) {
            std::optional<double> wd;
            if (l.align != "left")
                if (const auto r = textWidth(l.text, t.height, font, l.styles)) wd = *r;
            widths.push_back(wd);
            if (t.boxWidth <= 0)
                if (const auto wd = textWidth(l.text, t.height, font, l.styles)) block = std::max(block, *wd);
        }
        const Point2 along(std::cos(a), std::sin(a));
        for (size_t i = 0; i < lines.size(); ++i) {
            Primitive p;
            p.kind = PrimitiveKind::Text;
            p.position = Point2(t.position[0], t.position[1]) + double(i) * SHEET_TEXT_LINE_STEP * t.height * down;
            if (widths[i]) p.position += std::max(0.0, block - *widths[i]) * (lines[i].align == "center" ? 0.5 : 1.0) * along;
            p.height = t.height;
            p.angle = a;
            p.text = lines[i].text;
            p.styles = lines[i].styles;
            p.font = font;
            p.align = "left bottom";
            p.role = "text:" + t.id;
            out.primitives.push_back(std::move(p));
        }
    }
}

}  // namespace construe::drawing
