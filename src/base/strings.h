// Common string helpers (spec/base.md §7б).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace construe {

// Whitespace removed by trim() by default: space, tab, CR, LF.
inline constexpr std::string_view TRIM_WHITESPACE = " \t\r\n";

// s without leading and trailing characters from `chars` (bytewise; UTF-8 multibyte sequences are never touched,
// since all trimmed characters are ASCII). All characters trimmed — empty string.
std::string trim(const std::string& s, std::string_view chars = TRIM_WHITESPACE);

// Line and column (both from 1) of byte offset pos in UTF-8 text: lines are separated by '\n', the column counts code
// points (continuation bytes 10xxxxxx are not counted). pos beyond the text — the position after its last character.
std::pair<int, int> lineColumn(const std::string& text, size_t pos);

}  // namespace construe
