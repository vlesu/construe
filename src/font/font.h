// TrueType fonts and text outlines (spec/font.md, S2-SK-013): font library, reading glyf outlines,
// layout of a single-line text, approximation of quadratic curves by arcs and segments.
#pragma once

#include "base/error.h"
#include "base/math.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace construe::font {

inline constexpr double TEXT_CURVE_TOL_REL = 1e-3;  // font.md §7
inline constexpr double TEXT_CURVE_TOL_MIN = 1e-4;  // mm
inline constexpr int TEXT_APPROX_DEPTH = 10;
// Multi-line text and styles (font.md §4, §4а, FX8-SK-007), in cap heights of the font: baseline step between lines,
// synthesized bold outline shift per side, italic slant (tan 12°), underline top limit, gap below the lowest point of
// the line and thickness.
inline constexpr double TEXT_LINE_SPACING = 1.6;
inline constexpr double TEXT_BOLD_SHIFT = 0.03;
inline constexpr double TEXT_ITALIC_SLANT = 0.21255656167002213;
inline constexpr double TEXT_UNDERLINE_TOP = 0.15;
inline constexpr double TEXT_UNDERLINE_GAP = 0.1;
inline constexpr double TEXT_UNDERLINE_THICKNESS = 0.07;
inline constexpr double TEXT_BOLD_MAX_MITER = 4;  // bold: a control point moves by at most this many shifts

// Style of a fragment of a text (font.md §4а): code points [from, to) of the UTF-8 string (line breaks included).
struct StyleSpan {
    int from = 0, to = 0;
    bool bold = false, italic = false, underline = false;
    bool operator==(const StyleSpan&) const = default;
};

// Outline curve in sketch coordinates: segment a–b or counter-clockwise arc from a to b around center.
struct Curve2 {
    enum class Kind { Line, Arc };
    Kind kind = Kind::Line;
    Point2 a = Point2::Zero(), b = Point2::Zero(), center = Point2::Zero();
};

struct Layout {
    std::vector<std::vector<Curve2>> contours;
    std::vector<Error> warnings;  // FONT_MISSING_GLYPH (char)
};

class Font;
using FontPtr = std::shared_ptr<const Font>;

// Library directories (§2). By default — CONSTRUE_FONTS_DIR (separated by ';') and the repository font directories.
void setFontDirectories(std::vector<std::string> dirs);
std::vector<std::string> fontDirectories();
std::vector<std::string> defaultFontDirectories();  // default directories (independent of setFontDirectories)
// Distribution directories (§2а, S7-FONT-001): a font there is listed only with "<stem>.LICENSE.txt" next to it.
// By default — defaultFontDirectories(). loadFont still reads an unlisted font by its name.
void setLicensedFontDirectories(std::vector<std::string> dirs);
std::vector<std::string> licensedFontDirectories();
// Fonts excluded from lists and from default selection by name (§2а, S3-FONT-001): provenance not verified, even
// with a license file next to them. loadFont still reads them.
inline const std::vector<std::string> EXCLUDED_FONTS = {"DraughtsmanARegular.ttf", "DraughtsmanAItalic.ttf"};
bool isExcludedFont(const std::string& name);
std::vector<std::string> availableFonts();
// Default text font (§2): the preferred one (setPreferredFont) if it is in the library; otherwise the first of
// TEXT_DEFAULT_FONTS that is present; otherwise the first available.
inline const std::vector<std::string> TEXT_DEFAULT_FONTS = {"Sowjetschablone.ttf"};
void setPreferredFont(std::string name);  // empty — no preference
std::string preferredFont();
std::string defaultFont();
std::string builtinDefaultFont();  // the defaultFont rule without a preference (§2)
// Standard drawing text font (§2а, S3-FONT-001).
enum class DrawingStandard { Gost, Iso };
inline const std::vector<std::string> DRAWING_DEFAULT_FONTS_GOST = {"OpenGostTypeB-Regular.ttf", "OpenGostTypeA-Regular.ttf"};
inline const std::vector<std::string> DRAWING_DEFAULT_FONTS_ISO = {"osifont-lgpl3fe.ttf"};
const std::vector<std::string>& drawingDefaultFonts(DrawingStandard s);
std::string defaultDrawingFont(DrawingStandard s);
Result<FontPtr> loadFont(const std::string& name);
// Parsing font bytes (name is used in messages).
Result<FontPtr> parseFont(std::vector<uint8_t> bytes, const std::string& name);

// Layout of a text (§4): lines separated by '\n' (§4 item 6), styles of fragments (§4а).
Layout layoutText(const Font& f, const std::string& text, double height, const Point2& anchor, double angle,
                  const std::vector<StyleSpan>& spans = {});

// Font information (tests, debugging).
// Advance width of the glyph of a code point, font units (§4 item 3); nullopt — no glyph or the glyph is not supported
// (layoutText skips such a character without moving the pen). Text recognition (reconstruction.md §4.7а).
std::optional<double> advanceWidth(const Font& f, uint32_t codepoint);

int unitsPerEm(const Font& f);
double capHeight(const Font& f);  // in font units (§4 item 4)
std::string familyName(const Font& f);  // family name (name table, ID 1; §3.3), UTF-8; empty — no record

}  // namespace construe::font
