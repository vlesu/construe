// Strict JSON parsing (RFC 8259) and canonical serialization (spec/document.md §4.1, §5 stage 1).
#pragma once

#include "base/error.h"

#include <nlohmann/json.hpp>

#include <string>

namespace construe::doc {

using Json = nlohmann::ordered_json;

// Parsing: UTF-8 without BOM, a repeated key is an error. DOC_SYNTAX(position — bytes, detail).
Result<Json> parseJsonStrict(const std::string& text);

// Writing a JSON string by the canonical form rules (escaping §4.1).
std::string jsonString(const std::string& s);

}  // namespace construe::doc
