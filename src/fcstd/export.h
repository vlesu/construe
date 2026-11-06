// Export of a construe document to a FreeCAD 1.x document (spec/fcstd.md §12б–§12е): parts → PartDesign bodies with
// Sketcher sketches and features, imported bodies → Part::Feature, variables → spreadsheet "Variables"; what has no
// counterpart goes into the base body or is skipped, with a report entry.
#pragma once

#include "base/error.h"
#include "doc/document.h"
#include "fcstd/import.h"
#include "kernel/kernel.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace construe::fcstd {

struct ExportOptions {
    std::string element;  // ID of the element to export; empty — the whole document
};

// FreeCAD-side view of an exported sketch (for checks, spec/fcstd.md §12в.6): object name, our DoF and the number of
// written constraints by FreeCAD type name.
struct ExportedSketch {
    std::string feature;  // our sketch feature ID
    std::string object;   // FreeCAD object name
    int dof = -1;         // our DoF (weak dimensions are not constraints); −1 — unknown
    std::map<std::string, int> constraints;
};

// FreeCAD-side view of an exported assembly (§12з): links with our solved placements.
struct ExportedLink {
    std::string object;               // App::Link name
    std::array<double, 7> placement;  // x, y, z, qx, qy, qz, qw (as FreeCAD's Placement)
    bool fixed = false;               // grounded
    bool determined = false;          // its placement is fully determined by the mates (freeDof = 0)
};
struct ExportedAssembly {
    std::string element, object;  // our assembly ID, FreeCAD object name
    std::vector<ExportedLink> links;
};

struct ExportResult {
    std::string bytes;                // the .FCStd file
    std::vector<ReportEntry> report;  // object — our ID (element, feature, variable), feature — FreeCAD object name
    std::vector<ExportedSketch> sketches;
    // Expected bodies: FreeCAD body object name → (volume, area) of our final state.
    std::map<std::string, std::pair<double, double>> bodies;
    std::vector<ExportedAssembly> assemblies;
};

// Export (spec/fcstd.md §12б): pure function of the document and the kernel; the same document gives the same bytes.
// Errors: FCSTD_NOTHING_TO_EXPORT (no part or body to write), API_NOT_FOUND (options.element is not an element).
Result<ExportResult> exportDocument(const doc::Document& d, const kernel::Kernel& k, const ExportOptions& options = {});

}  // namespace construe::fcstd
