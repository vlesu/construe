// GOST 2.106 assembly specification (spec/specification.md): rows from the assembly composition, layout over A4 sheets,
// sheet primitives (frame and title block of form 2/2а — title_block.cpp).
#pragma once
#include "drawing/sheet.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace construe::drawing {

// Specification row (§3).
struct SpecItem {
    std::string section;  // section (§2)
    std::string key;      // automatic row — key (§3.1); manual — empty
    int extra = -1;       // manual row — index in extraRows; otherwise −1
    bool positionSet = false;  // position set explicitly (override or manual row, §3.5)
    // Automatic composition row: its instances — top-level instance IDs, connection fasteners — derived instance paths
    // "<feature>/<output>" (§3.1 item 2а, FX14-SPEC-001); otherwise empty.
    std::vector<std::string> instances;
    std::string format, zone, position, designation, name, quantity, note;
};

struct SpecContent {
    std::vector<SpecItem> items;  // in final order (§3.4) with positions (§3.5)
    std::vector<Error> errors;    // §7
};

// Document of another file by the instance reference path (as in the document); nullptr — not read (§3).
using LinkedDocs = std::function<const doc::Document*(const std::string& file)>;

// Sections in GOST 2.106 order (§2) and their headings; unknown section — empty.
const std::vector<std::string>& specSections();
std::string specSectionTitle(const std::string& section);
// Natural string comparison (§3.4): digit runs — by number.
bool naturalLess(const std::string& a, const std::string& b);

// Connection reports of the assembly by feature ID (PartState.connections of its final state, solutions.md §7.7).
using SpecConnections = std::map<std::string, doc::Json>;

// Specification rows (§3); connections — the reports of the specification's assembly for the fastener rows (§3.1 item
// 2а); nullptr — not known (no fastener rows).
SpecContent specificationContent(const doc::Document& d, const doc::Element& spec, const LinkedDocs& linked,
                                 const SpecConnections* connections = nullptr);

// Table line (§4.1).
struct SpecLine {
    enum class Kind { Blank, Heading, Item };
    Kind kind = Kind::Blank;
    std::string section;     // Heading
    int item = -1;           // Item: index in SpecContent::items
    bool first = false;      // Item: first line of the row (all columns)
    std::string name, note;  // Item: name and note texts of this line (wrapping)
};
using SpecPages = std::vector<std::vector<SpecLine>>;
// Layout of rows over sheets (§4.1, §4.2); sheets ≥ 1.
SpecPages specificationPages(const SpecContent& c);

// Parts list on a drawing sheet (drawing.md §6.28, FX14-DRW-003): the table lines (§4.1 without the leading and trailing
// empty lines), its size (width — the columns «Pos.», «Designation», «Name», «Qty», «Note» of form 1; height — the header
// and the lines) and its primitives with the top-left corner at topLeft, all with the role `role`.
std::vector<SpecLine> partsListLines(const SpecContent& c);
SheetSize partsListSize(const SpecContent& c);
void appendPartsListTable(const SpecContent& c, const Point2& topLeft, const std::string& role, SheetGraphics& g);

// Keys of the specification title block fields (§5) and default values of sheet page (0…pages−1).
const std::vector<std::string>& specificationTitleKeys();
std::map<std::string, std::string> specificationTitleDefaults(const doc::Document& d, const doc::Element& spec, int page, int pages);

// Primitives of sheet page (0…pages.size()−1) (§4.3, §5); build errors c.errors — into the sheet errors (empty view).
SheetGraphics composeSpecificationPage(const doc::Document& d, const doc::Element& spec, const SpecContent& c, const SpecPages& pages,
                                       int page);

// ID of a virtual specification sheet: "page1", "page2", … (core-api §4.1е); specPageIndex — number from 0 or −1.
std::string specPageId(int page);
int specPageIndex(const std::string& id);

// Constants (§6).
inline constexpr double SPEC_ROW_H = 8, SPEC_HEADER_H = 15;
inline constexpr int SPEC_ROWS_FIRST = 29, SPEC_ROWS_NEXT = 32;
inline constexpr double SPEC_TEXT_H = 3.5;
inline constexpr double SPEC_UNDERLINE_GAP = 0.8;
inline constexpr int SPEC_RESERVE_ROWS = 1;

}  // namespace construe::drawing
