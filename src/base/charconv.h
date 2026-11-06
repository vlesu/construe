// Locale-independent parsing of floating-point numbers (spec/base.md §2б, S8-PKG-003).
#pragma once

#include <system_error>

namespace construe {

struct ParseDoubleResult {
    const char* ptr;  // first character not parsed
    std::errc ec;     // {} — parsed; invalid_argument — no number at `first` (ptr == first); result_out_of_range
};

// On result_out_of_range `value` is ±inf (overflow) or ±0 (underflow) on every platform — unlike std::from_chars,
// whose value there differs between standard libraries; on invalid_argument `value` is unchanged.

// std::from_chars(first, last, value) for double in the general format: the same accepted syntax, result and errors
// (the value on out-of-range — above).
// libc++ of Apple's SDK has no floating-point from_chars, so there the call goes to parseDoubleStrtod.
ParseDoubleResult parseDouble(const char* first, const char* last, double& value);
// The portable implementation over strtod in the "C" locale with from_chars rules (no leading spaces or '+', no
// hexadecimal); always built so that tests compare it with std::from_chars where the latter exists.
ParseDoubleResult parseDoubleStrtod(const char* first, const char* last, double& value);

}  // namespace construe
