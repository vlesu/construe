// Variable types and variant tables: helpers of the variable commands (core-api.md §4.3, §4.3а).
#pragma once

#include "base/error.h"
#include "doc/document.h"
#include "doc/table_text.h"

#include <string>
#include <vector>

namespace construe::api {

// Type and list values check (setVariableType): API_INVALID_ARGUMENT.
Status checkVariableType(const std::string& type, const std::vector<std::string>& values);
// The variable is a variant table column: the text goes to the active row cell (true); otherwise false.
bool setVariantCell(doc::Document& d, const std::string& variableId, const std::string& text);
// Removes the variable's column from its table (a table without columns is removed).
void removeVariantColumn(doc::Document& d, const std::string& variableId);

// Text of a table (§4.3а) — doc/table_text.h.
using doc::TableText;
using doc::parseTableText;
using doc::writeTableText;

}  // namespace construe::api
