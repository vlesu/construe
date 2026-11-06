// Number notation (spec/base.md §7г): the canonical form of the document (spec/document.md §4.1) and the shared
// fixed-precision and round-trip forms used by texts, drawings and exported files.
#pragma once

#include <string>

namespace construe {

// The ECMAScript Number::prototype.toString rule — the shortest notation that restores the number unambiguously;
// -0 → "0"; NaN/Inf are not allowed.
std::string canonicalNumber(double value);

// Fixed notation with `digits` (0…17) digits after the point, rounding the exact binary value (ties as in the exact
// decimal expansion), then trailing fractional zeros and a bare point removed; a result "-0" becomes "0".
// Examples (3 digits): 1.5 → "1.5", 2.0 → "2", -0.0004 → "0", 0.0005 → "0.001", 1e21 → "1000000000000000000000".
std::string fixedTrimmed(double v, int digits);
// Removes trailing zeros of the fractional part and a bare decimal point ("1.500" → "1.5", "2.000" → "2", "10" → "10").
void trimFractionZeros(std::string& s);
// %.17g — enough digits to restore any double; ±0 → "0".
std::string roundTrip(double v);

// Decimal comma for display: every '.' → ','.
std::string decimalComma(std::string s);
// Display form of a number text: decimal comma and the typographic minus "−" (U+2212) instead of '-'.
std::string localizeNumber(const std::string& s);

}  // namespace construe
