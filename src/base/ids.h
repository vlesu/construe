// Comparison of document identifiers (spec/base.md §4).
#pragma once

#include <string_view>

namespace construe {

// true if the string has the form <prefix [a-z]+><number [0-9]+ without leading zeros>.
bool isNumberedId(std::string_view id);

// true if a numbered ID or a segment piece ID <numbered>@<number> (spec/base.md §4).
bool isSegmentId(std::string_view id);

// numbered-ID order: prefix bytewise, then number as an integer, then piece number (no piece — earlier).
// For strings of any other form — a programming error (assert).
int compareIds(std::string_view a, std::string_view b);

// Comparator for std::sort / std::map.
struct IdLess {
    bool operator()(std::string_view a, std::string_view b) const { return compareIds(a, b) < 0; }
};

}  // namespace construe
