// 3D technical requirements (spec/pmi.md): ISO 286 tolerance tables, limit deviations, designation rules,
// text formatting and symbol geometry. Stateless pure functions.
#pragma once

#include "base/error.h"
#include "base/math.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace construe::pmi {

using Json = nlohmann::ordered_json;

inline constexpr double FIT_NOMINAL_ROUND = 1e-6;  // mm (§7)
inline constexpr double FIT_MAX_NOMINAL = 500;      // mm
inline constexpr double FIT_SMALL_NOMINAL = 1;     // mm: restrictions for nominal sizes up to 1 mm (§4.3)
inline constexpr int DEV_DECIMALS = 4;
inline constexpr int SIZE_DECIMALS = 3;
inline constexpr int ROUGHNESS_DECIMALS = 3;

struct Deviations {
    double upper = 0, lower = 0;  // mm
};

struct FieldParts {
    std::string letters;
    int grade = 0;
    bool hole = false;
};

// §4.1–§4.3.
Result<FieldParts> parseField(const std::string& field);
Result<double> standardTolerance(int grade, double nominal);
Result<Deviations> fieldDeviations(const std::string& field, double nominal);
// §4.3а (FX13-TOL-001): preferred fields for the tolerance field list, frequent first in each group.
inline const std::vector<std::string> STANDARD_HOLE_FIELDS = {"H7", "H8", "H9", "H11", "G7", "F8", "E9", "D10", "JS7", "K7", "M7", "N7", "P7"};
inline const std::vector<std::string> STANDARD_SHAFT_FIELDS = {"h6", "h7", "h8", "h9", "h11", "g6", "f7", "e8", "d9", "js6", "k6", "m6", "n6", "p6", "r6", "s6"};
const Json& fitTables();

// §2.2–§2.3: designation rules.
bool characteristicKnown(const std::string& c);
bool needsDatums(const std::string& c);   // orientation, location, runout
bool allowsDatums(const std::string& c);  // the same (form types — without datums)
bool allowsDiameter(const std::string& c);
bool allowsMaterial(const std::string& c);
const std::vector<std::string>& characteristics();  // in the order of the §2.3 table
std::string characteristicName(const std::string& c);
bool toleranceDimension(const std::string& constraintType);  // the dimension admits a tolerance
bool sizeDimension(const std::string& constraintType);       // size of a feature: admits a tolerance zone
// Designation letter series (GOST R 2.316-2023 cl. 5.9: without Ё, З, Й, О, Х, Ч, Ъ, Ы, Ь), 24 letters — sections,
// detail views and datums (§2.4).
const std::vector<std::string>& designationLetters();
// Suggested datum letter (§2.4): the first letter of designationLetters not in used; all used — "А".
std::string nextDatumLetter(const std::vector<std::string>& used);
bool validDatumLetter(const std::string& s);                  // 1–2 capitals: A–Z, Cyrillic U+0410–U+042F, U+0401
std::string laySymbol(const std::string& lay);

// §5: texts.
std::string formatNumber(double v, int decimals);
std::string formatDeviation(double v);
std::string deviationsText(const Deviations& d);
std::string toleranceSuffix(const Json& fit, const std::optional<Deviations>& d);
// The label of a tolerance in the part and the assembly (FX13-TOL-002): a field always with its deviations when known.
std::string toleranceLabel(const Json& fit, const std::optional<Deviations>& d);
std::string roughnessText(const Json& a);
std::string frameValueText(const Json& a);
// §5.4 (FX12-PMI-004): tolerance written in the dimension field — the inverse of toleranceSuffix. Empty — no tolerance
// (nullopt); otherwise the tolerance fields of a `fit` ({field[, showDeviations]} or {upper, lower}); PMI_FIELD_SYNTAX.
Result<std::optional<Json>> parseToleranceText(const std::string& text);

// §6: symbol geometry.
struct Glyph {
    struct Circle {
        Point2 center = Point2::Zero();
        double r = 0;
    };
    std::vector<std::vector<Point2>> polylines;
    std::vector<Circle> circles;
    std::vector<std::vector<Point2>> filled;
};
Glyph characteristicGlyph(const std::string& c);
Glyph roughnessGlyph(const std::string& sign, double textWidth, bool shelf);
// §6.2 roughness sign proportions (GOST 2.309-73 ed. 2003, FX12-PMI-006), in units of the text height h.
inline constexpr double ROUGHNESS_LEG_DEG = 60;                 // both legs to the horizontal (60° between them)
inline constexpr double ROUGHNESS_SHORT_H = 1, ROUGHNESS_LONG_H = 3;  // h and H = 3h (GOST: H = (1.5…5)h)
inline constexpr double ROUGHNESS_SHORT_LEG_X = -0.577, ROUGHNESS_LONG_LEG_X = 1.732;  // leg ends: h·cot 60°, H·cot 60°
inline constexpr double ROUGHNESS_SHELF_PAD = 0.4;              // shelf beyond the widest text
inline constexpr double ROUGHNESS_LEADER_GAP = 0.5;             // leader shelf before the short leg end
// Text line places (lower left corner of a line of height h): parameter under the shelf, method above it, lay sign
// at the lower right of the sign.
inline const Point2 ROUGHNESS_PARAMETER_AT{1.9, 1.8}, ROUGHNESS_METHOD_AT{1.9, 3.3}, ROUGHNESS_LAY_AT{0.9, 0.1};

}  // namespace construe::pmi
