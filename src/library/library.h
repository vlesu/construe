// Library of standard parts (spec/library.md): package loading and checks, library folders with priority, catalog
// search, the document copy of a package, designation rule, saving a new package. Pure functions over the disk.
#pragma once

#include "base/error.h"
#include "doc/document.h"
#include "expr/expr.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::library {

inline constexpr int LIB_FORMAT_VERSION = 1;
inline constexpr size_t LIB_ID_MAX = 128;
inline constexpr const char* LIB_SIZES_TABLE_NAME = "Типоразмеры";
inline constexpr const char* LIB_DESCRIPTION_FILE = "library-part.json";
inline constexpr const char* LIB_TEMPLATE_DEFAULT = "template.construe";
inline constexpr const char* LIB_SIZES_DEFAULT = "sizes.csv";
inline constexpr const char* LIB_ISO_COLUMN = "iso";  // ISO designation of a size (§2.2a)

struct StandardRef {
    std::string code;
    std::optional<int> year;
    std::string designation;  // rule by this standard (§2.1; reserved, §10)
    // "code-year" or code.
    std::string full() const;
};

struct Source {
    std::string standard, table, note;
};

// Named attachment of an element (§2.4, FX10-LIB-003): a place of the element the user points assembly geometry to.
struct Attachment {
    std::string name;
    std::string kind;  // "axis", "plane", "point"
    std::string ref;   // template geometry in mate reference form without the instance path
    bool flip = false;  // the direction of ref reversed (a plane: the outward normal against the ref normal)
    std::string label;
    std::string mate;  // "concentric" / "coincident"; empty — by kind
    bool optional = false;
    std::string autoRule;  // "shoulder" or empty
    bool matchDiameter = false;
    std::string diameterKey;
    // Mate type: the field or the default of the kind.
    std::string mateType() const { return !mate.empty() ? mate : kind == "axis" ? "concentric" : "coincident"; }
};

// Cut of an element (§2.5, FX10-LIB-009): a tool body in template coordinates by expressions of the template variables,
// subtracted from the part the user pointed the attachment `attachment` to.
struct Cut {
    using Range = std::pair<std::string, std::string>;  // expressions [a, b] / [r, z]
    std::string name, title, attachment;
    std::vector<Range> revolve;      // half profile (r, z) about the template axis Y; empty — a box
    std::optional<std::array<Range, 3>> box;  // x, y, z ranges in template axes
};

// Package description library-part.json (§2.1).
struct Description {
    std::string id, title, description;
    std::vector<std::string> section;
    std::vector<StandardRef> standards, equivalents;
    std::string icon, templateFile = LIB_TEMPLATE_DEFAULT, element, sizesFile = LIB_SIZES_DEFAULT;
    std::vector<std::string> keyVariables;
    std::string defaultSize, designation;
    bool isoAnalog = false;  // the ISO designations (column iso, §2.2a) are analogs
    std::vector<Source> source;
    std::vector<Attachment> attachments;  // §2.4
    std::vector<Cut> cuts;                // §2.5
};

struct SizeRow {
    std::string name;
    std::vector<std::string> values;  // a cell per column
};
struct Sizes {
    std::vector<std::string> columns;  // variable names of the template
    std::vector<SizeRow> rows;
    int column(const std::string& name) const;  // −1 — none
};

struct Package {
    std::string dir;  // absolute, '/'
    Description description;
    doc::Document templateDoc;
    std::string element;  // the part of the template
    Sizes sizes;
    std::string fingerprint;  // §3
    std::string iconPath;     // absolute or empty
};

// Package check and loading (§2.3): the first error with its place (file, line, column, field, name, dir).
Result<Package> loadPackage(const std::string& dir);
// Canonical JSON text of a description (§8).
std::string writeDescription(const Description& d);

// Library folders (§4): CONSTRUE_LIBRARY_DIR (';'-separated), then the program's assets/library.
std::vector<std::string> defaultLibraryDirectories();

struct CatalogItem {
    Package package;
    std::string folder;      // library folder the package was found in
    std::string shadowedBy;  // dir of the effective package with the same id; empty — this one is effective
    std::vector<std::string> designations;  // designation of each size row (§7), in row order
};
struct Catalog {
    std::vector<CatalogItem> items;  // in traversal order
    std::vector<Error> problems;
    // The effective package with this id; nullptr — none.
    const CatalogItem* find(const std::string& id) const;
};
Catalog scan(const std::vector<std::string>& folders);

struct Translations;  // §17, below

// Search normalization and search (§5).
std::string normalizeSearch(const std::string& s);
// With a dictionary of another language the translated titles and sections and the shown standard codes are searched
// too (§17.2).
std::vector<size_t> search(const Catalog& catalog, const std::string& query, const Translations* tr = nullptr);
// The size row found by the query (§5): the first row whose name, designation or ISO designation match every word.
std::optional<std::string> searchRow(const CatalogItem& item, const std::string& query);

// Languages of the library texts (§17, FX14-I18N-001): the dictionary of a language — source (Russian) text →
// translation, from the files i18n/<language>.tsv in the roots of the library folders.
struct Translations {
    std::string language;                      // "ru" — the source language, an empty dictionary
    std::map<std::string, std::string> texts;  // source text → translation
};
// Entries of a dictionary text (§17.1): (source, translation) in file order; lines without a tab or with an empty
// translation are skipped. Escapes \n, \t, \\ and the double quotes around a text are resolved.
std::vector<std::pair<std::string, std::string>> parseTranslations(const std::string& text);
// The dictionary of `language` by the ordered folders (§4 priority): the first folder that has a key wins.
Translations loadTranslations(const std::vector<std::string>& folders, const std::string& language);
// The translation of `text`, otherwise `text` itself (fallback to Russian, §17.2).
const std::string& translate(const Translations& tr, const std::string& text);
// Standard code for display (§17.2): in "ru" as is, otherwise the Cyrillic words (GOST, R, ISO …) in Latin, word by word.
std::string standardText(const std::string& code, const std::string& language);

// Template variable values with row `row` of the sizes table active (§7) and the designation of that row.
expr::VariableTable rowVariables(const Package& p, const std::string& row);
std::string rowDesignation(const Package& p, const std::string& row);
std::string designationRule(const Package& p);
// Numbers of a cut (§2.5) at row `row`, mm: the expressions by rowVariables; an expression error, r < 0 or a ≥ b —
// API_ATTACH_CUT (name — the cut, detail — the expression error code, "radius" or "range").
struct CutTool {
    std::vector<std::array<double, 2>> revolve;            // (r, z)
    std::optional<std::array<std::array<double, 2>, 3>> box;  // x, y, z: [a, b]
};
Result<CutTool> cutTool(const Package& p, const Cut& c, const std::string& row);
// ISO designation of the row (§2.2a): the trimmed cell of the column iso; empty — none.
std::string rowIso(const Package& p, const std::string& row);

// Standard filter of the catalog view (§2.7, FX11-STD-002, FX11-STD-003): which standards the user enabled.
struct StandardFilter {
    bool gost = true, iso = true;
    bool both() const { return gost && iso; }
    bool isoOnly() const { return iso && !gost; }
};
enum class StandardFamily { None, Gost, Iso };
// Family of a standard code: the GOST prefix — Gost (GOST R ISO included), the ISO prefix (Latin or Cyrillic) — Iso.
StandardFamily standardFamily(const std::string& code);
struct PackageFamilies {
    bool gost = false, iso = false;  // iso: an ISO code or a non-empty ISO designation of a row
};
PackageFamilies packageFamilies(const Package& p);
// The element is shown under the filter: both enabled, no family, or one of its families enabled.
bool visibleUnder(const Package& p, StandardFilter f);
// Standards then equivalents except those of a disabled family.
std::vector<StandardRef> shownStandards(const Description& d, StandardFilter f);
// Row indices shown under the filter: «ISO only» with the column iso — rows with an ISO designation; otherwise all.
std::vector<size_t> shownRows(const Package& p, StandardFilter f);
// The row name shown: «ISO only» with the column iso — its ISO designation; otherwise the row name.
std::string shownRowName(const Package& p, size_t row, StandardFilter f);

// Document copy of the package (§6.1).
doc::LibraryPart embed(const Package& p);

// Values offered for key variable index k given the chosen values of keys 0..k-1 (§6.2); and the row of a full choice.
std::vector<std::string> keyValues(const Package& p, size_t k, const std::vector<std::string>& chosen);
std::optional<std::string> rowOfKeys(const Package& p, const std::vector<std::string>& keys);

// Template for saving (§8): the document with only part element, without library copies, columns of the sizes table
// removed from the template's variant tables.
doc::Document templateForSave(const doc::Document& d, const std::string& element, const std::vector<std::string>& sizeColumns);
// Writes a new package parent/folder (§8) and checks it with loadPackage; on error the folder is removed.
Result<std::string> savePackage(const std::string& parent, const std::string& folder, const Description& def,
                                const doc::Document& templateDoc, const std::string& sizesCsv);

}  // namespace construe::library
