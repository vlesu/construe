// Export of a drawing sheet to PDF, SVG (spec/io.md §2в; S3-IO-001, S3-IO-002) and DXF (§2г; S3-IO-003).
#pragma once

#include "base/error.h"
#include "drawing/sheet.h"

#include <string>
#include <vector>

namespace construe::io {

enum class SheetFileFormat { Pdf, Svg, Dxf };

// File content (pure functions; errors come only from the text font) and atomic write.
Result<std::string> sheetPdf(const drawing::SheetGraphics& g);
Result<std::string> sheetSvg(const drawing::SheetGraphics& g);
Result<std::string> sheetDxf(const drawing::SheetGraphics& g);  // io/sheet_dxf.cpp
Status writeSheetFile(const std::string& path, const drawing::SheetGraphics& g, SheetFileFormat format);
// Several sheets — one PDF page each, in order (§2в, FX7-DRW-005); one page — the same bytes as sheetPdf.
Result<std::string> sheetsPdf(const std::vector<drawing::SheetGraphics>& pages);
Status writeSheetsPdf(const std::string& path, const std::vector<drawing::SheetGraphics>& pages);
// File of sheet i (from 0) of a DXF export by sheets (§2г «По листам», FX11-DXF-001): the first — path, the next —
// "-2", "-3", … before the extension (the part after the last '.' of the file name; none — at the end).
std::string sheetFilePath(const std::string& path, size_t i);
// DXF by sheets: one file per sheet, names by sheetFilePath; all bytes are prepared before the first file is written.
Status writeSheetsDxf(const std::string& path, const std::vector<drawing::SheetGraphics>& sheets);

// Number in PDF/SVG output (§2в): at most 4 digits after the point, no trailing zeros, "-0" → "0".
std::string sheetNumber(double x);

}  // namespace construe::io
