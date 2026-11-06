// Import of a FreeCAD document into a construe document (spec/fcstd.md §5–§10): PartDesign bodies → parts with
// sketches and features, objects without a counterpart → base body "Import", Spreadsheet aliases → variables.
#pragma once

#include "base/error.h"
#include "doc/document.h"
#include "fcstd/fcstd.h"
#include "kernel/kernel.h"

#include <string>
#include <vector>

namespace construe::fcstd {

// Tolerances of the import (spec/fcstd.md §12).
inline constexpr double FCSTD_GEOM_TOL = 1e-6;         // mm: coincidence of planes, points on edges and faces
inline constexpr double FCSTD_MATCH_TOL = 1e-5;        // mm (× max(1, size)): centroids and lengths when matching
inline constexpr double FCSTD_VOLUME_REL_TOL = 1e-6;   // relative: body volume and area equal to FreeCAD's shape
inline constexpr double FCSTD_VALUE_TOL = 1e-7;        // mm or rad: a dimension value equals the measured one
inline constexpr int FCSTD_VALUE_DIGITS = 9;           // decimals of imported numbers (mm, deg)

// Outcome of an object (spec/fcstd.md §9): the groups of the exchange report.
enum class ReportKind { Transferred, Approximate, BaseBody, Skipped };
const char* reportKindName(ReportKind k);

struct ReportEntry {
    std::string object;  // FreeCAD object name
    std::string label;   // its Label
    std::string type;    // FreeCAD type
    ReportKind kind = ReportKind::Transferred;
    std::vector<std::string> reasons;  // reason codes with details (spec/fcstd.md §9), empty for Transferred
    std::string feature;               // ID of the resulting feature or element (empty — none)
};

struct ImportResult {
    doc::Document document;
    std::vector<ReportEntry> report;  // in FreeCAD document order
};

// Import (spec/fcstd.md §5): pure function of the model and the kernel. Errors: FCSTD_NO_OBJECTS (nothing to import).
Result<ImportResult> importDocument(const FcDocument& fc, const kernel::Kernel& k);

// Reading and import of file content (readFcstd + importDocument).
Result<ImportResult> importFcstd(const std::string& bytes, const kernel::Kernel& k);

}  // namespace construe::fcstd
