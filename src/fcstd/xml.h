// Minimal XML reader for FreeCAD Document.xml (spec/fcstd.md §4.1): elements, attributes, entities; no DTD,
// no namespaces, character data between elements is ignored (the format keeps all values in attributes).
#pragma once

#include "base/error.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace construe::fcstd::xml {

struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;  // in file order, entities decoded
    std::vector<Element> children;
    int line = 0;  // 1-based line of the start tag

    // Attribute value or nullptr.
    const std::string* attr(const std::string& key) const;
    std::string attrOr(const std::string& key, const std::string& fallback = {}) const;
    // First child with the name or nullptr.
    const Element* child(const std::string& n) const;
};

// Parses a whole document: the root element. Errors — FCSTD_BAD_XML (`detail`, `line`).
Result<Element> parse(const std::string& text);

}  // namespace construe::fcstd::xml
