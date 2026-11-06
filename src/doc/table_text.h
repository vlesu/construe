// Text of a variant table (spec/core-api.md §4.3а): header names and rows; CSV or tab-delimited (clipboard).
#pragma once

#include "base/error.h"
#include "doc/document.h"

#include <string>
#include <vector>

namespace construe::doc {

struct TableText {
    std::vector<std::string> header;  // variable names (without the first field)
    std::vector<VariantRow> rows;
};
std::string writeTableText(const TableText& t, char delimiter);
// Errors — API_TABLE_TEXT with line, column, reason (§4.3а).
Result<TableText> parseTableText(const std::string& text);

}  // namespace construe::doc
