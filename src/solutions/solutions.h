// Library of standard methods (spec/solutions.md): solution package loading and checks (§3), folders (§4), the document
// copy (§5), evaluation of choices, rules and outputs over measured inputs and tables (§6). Pure functions: geometry
// (input resolution, measurement, cuts, placements) is done by regen (§7).
#pragma once

#include "base/error.h"
#include "doc/document.h"
#include "expr/expr.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace construe::solutions {

inline constexpr int SOL_FORMAT_VERSION = 1;
inline constexpr const char* SOL_DESCRIPTION_FILE = "solution.json";
inline constexpr double SOL_EQ_TOL = 1e-9;             // relative (§9)
inline constexpr double SOL_PERP_TOL = 1e-6;           // |sin| (§9)
inline constexpr double SOL_PROBE_MIN_VOLUME = 1e-6;   // mm³ (§9)
inline constexpr double SOL_PACK_GAP = 1e-3;           // mm (§9)
inline constexpr int SOL_PREVIEW_PRECISION = 6;        // §9
inline constexpr double SOL_SYNC_TOL = 1e-9;           // mm; materialization of owned cuts (§7.10, §9)
inline constexpr int SOL_SYNC_WAIT_MS = 60000;         // saving waits for the assembly regeneration (§7.10, §9)

// Own table of a package (§3.3, file): columns and rows as text.
struct DataTable {
    std::vector<std::string> columns;
    std::vector<doc::VariantRow> rows;
};

struct InputDef {
    std::string id, type, title, face;  // type: face | cylinder | point; face — of a point input
    bool optional = false;
};
struct ChoiceDef {
    std::string id, title, type;  // table | list | value | bool | length
    std::vector<std::string> options, labels;
    std::string fromTable, fromColumn, label;
    doc::Json defaultValue;  // null — none
    std::string variants;    // bool only: the table choice shown with it as one list «no» + its variants (§3.4)
};
struct TableDef {
    std::string id, title;
    std::string library, file, builtin;  // exactly one is set
};

// Parsed description (the JSON is validated by loadSolution / checkDefinition).
struct Definition {
    std::string id, title, description;
    std::vector<std::string> section;
    std::vector<InputDef> inputs;
    std::string frameFace, frameAxis, framePoint;
    std::string framePackEnd;  // face input of the pack end (FX8-BLT-003, §3.2); empty — the pack by the probe only
    std::optional<std::string> probe;
    std::vector<ChoiceDef> choices;
    std::vector<TableDef> tables;
    doc::Json rules = doc::Json::array(), outputs = doc::Json::array();
    const InputDef* input(const std::string& id) const;
    const ChoiceDef* choice(const std::string& id) const;
    const TableDef* table(const std::string& id) const;
};

struct Solution {
    std::string dir;  // absolute, '/'
    doc::Json definition;
    std::map<std::string, DataTable> files;  // own tables by table id
    std::string fingerprint;                 // §5.1
};

// Package check and loading (§3.5): the first error with its place.
Result<Solution> loadSolution(const std::string& dir);
// Checks 4–11 and 14 of §3.5 over a definition (an embedded copy): SOL_* with field, name.
Status checkDefinition(const doc::Json& definition);
Definition parseDefinition(const doc::Json& definition);

// Folders (§4): the same traversal and priority as the library of standard parts.
struct CatalogItem {
    Solution solution;
    std::string folder, shadowedBy;
};
struct Catalog {
    std::vector<CatalogItem> items;
    std::vector<Error> problems;
    const CatalogItem* find(const std::string& id) const;
};
Catalog scan(const std::vector<std::string>& folders);

// Document copy (§5.2).
doc::SolutionCopy embed(const Solution& s);
// Own tables of a copy by table id.
std::map<std::string, DataTable> copyTables(const doc::SolutionCopy& c);

// ------------------------------------------------------------------ evaluation (§6)

// A table for evaluation: rows with cell values by column, cell texts (choices of type value) and designations.
struct TableRow {
    std::string name;
    std::vector<std::string> texts;              // per column
    std::vector<Result<expr::Quantity>> values;  // per column
    std::string designation;                     // library tables: designation of the row (library.md §7)
};
struct TableData {
    std::vector<std::string> columns;
    std::vector<TableRow> rows;
    int column(const std::string& name) const;
};
// Provider of tables by table id; nullopt — the table is unavailable (library copy missing: SOL_LIBRARY_NOT_FOUND).
using TableProvider = std::function<std::optional<TableData>(const std::string& tableId)>;

// Own table (§3.3 file): cells are expressions in mm and degrees without variables.
TableData fileTable(const DataTable& t);
// The built-in hole table (§3.3 builtin "holes") from the parsed hole-tables.json.
TableData holesTable(const doc::Json& holeTables);

struct Value {
    std::string name;
    expr::Quantity q;
};
struct RowPick {
    std::string rule, table, row;
};
struct CutPlan {
    std::string id, title, parts;
    int index = 0;  // 1-based position of the output in the description (face names, solutions.md §7.5)
    bool revolve = true;
    std::vector<std::pair<double, double>> polygon;  // revolve: (r, z)
    std::array<double, 6> box{};                     // box: x0, x1, y0, y1, z0, z1
    std::optional<std::array<double, 3>> thread;     // d, pitch, length
    std::string fit;
};
struct PartPlan {
    std::string id, title, table, row, designation;
    double x = 0, y = 0, z = 0;
    bool in = true;
};
struct Plan {
    std::vector<Value> values;  // choices, measurements, let — in evaluation order (a let repeat replaces in place)
    std::vector<RowPick> rows;
    std::vector<CutPlan> cuts;
    std::vector<PartPlan> parts;
    std::optional<Error> error;
};

// Choice values (§6.4): the stored choice or its default, as JSON (string / boolean).
std::map<std::string, doc::Json> choiceValues(const Definition& def, const doc::Json& stored);
// Variants of a choice (§3.4, §8): options of table/list, cell texts of value (with the current values of the earlier
// choices), true/false of bool; empty — length. Labels — parallel.
std::vector<std::pair<std::string, std::string>> choiceOptions(const Definition& def, const ChoiceDef& c,
                                                               const std::map<std::string, doc::Json>& values,
                                                               const TableProvider& tables);

// A table choice that is the source table of a value choice (from.table = "$<id>", §3.4): the type of the part whose
// size the value choice picks (the fastener type of the bolted joint). Its variants are always offered (§8, FX13-JNT-003).
bool isTypeChoice(const Definition& def, const ChoiceDef& c);
// The choices after choice `changed` got a new value (§8, FX13-JNT-003): every value choice taking its cells from the table
// of `changed` goes to its default, if the new table has it, else to the cell of that column nearest to the default by
// value (the first of equal ones; no default — to the current value, else the first cell). Stored choices otherwise as
// given; the result stores the dependent choices explicitly.
doc::Json resetDependentChoices(const Definition& def, const doc::Json& stored, const std::string& changed, const TableProvider& tables);

// Measurement callbacks: inputs (cylinder diameter and length) are given; the pack is measured with probe radius ρ (§7.4).
using PackMeasure = std::function<Result<std::vector<Value>>(double probeRadius)>;

// Evaluation §6.1: choices, input measurements, pack, rules, outputs.
Plan evaluate(const Definition& def, const doc::Json& storedChoices, const std::vector<Value>& inputMeasures,
              const TableProvider& tables, const PackMeasure& pack);

// ------------------------------------------------------------------ recording a solution in the program (§8а)

struct RecordedRule {
    std::string kind;  // let | select
    std::string name, expr;                    // let: the expression; select: the right side of the condition
    std::string table, column, op, order;      // select; order: "" | asc | desc
};
struct RecordedOutput {
    std::string kind;  // cut | part
    std::string title;
    std::string parts, tool, geometry;              // cut: tool — revolve | box; geometry — the tool text (§8а.2)
    std::string table, row, z, direction = "in";    // part
};
struct Recording {
    std::string id, title, description;
    std::vector<std::string> section;
    std::vector<InputDef> inputs;  // id, type, title (face of a point is derived)
    std::optional<std::string> probe;
    std::vector<TableDef> tables;  // library or builtin
    std::vector<RecordedRule> rules;
    std::vector<RecordedOutput> outputs;
};

// The description of a recording (§8а.2), checked by checkDefinition; SOL_RECORD_FRAME, SOL_TOOL_TEXT.
Result<doc::Json> recordDefinition(const Recording& r);
// Writes the package parent/folder with solution.json and checks it by loadSolution (§8а.3); returns the folder.
Result<std::string> saveSolution(const std::string& parent, const std::string& folder, const doc::Json& definition);

}  // namespace construe::solutions
