// Expression field input normalization (spec/app-ux.md §5.6, S7F-RF-006): one rule for every expression field.
#pragma once

#include <QString>

#include <optional>
#include <string>

namespace construe::app {

// Text of an expression field as stored in the document (§5.6): trimmed, a leading "=" removed, a single number with a
// decimal comma and an optional unit gets a point ("2,5in" → "2.5 in"); otherwise unchanged — a comma separates
// function arguments ("max(2, 3)").
std::string normalizeExprInput(const QString& text);

// The normalized text is one number with an optional unit ("10", "-2.5 in", "1e3 mm") — no variables or operations.
bool isPlainNumber(const std::string& normalized);

// A plain number field (not an expression: text height, tolerance, material, §5.6) with a dot or a comma and an
// optional "−" (U+2212); nullopt — not a finite number.
std::optional<double> parseDecimal(const QString& text);

}  // namespace construe::app
