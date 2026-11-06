// Assembly weld seams (spec/weld.md): weld type table, weld record check, GOST 2.312 label and symbol geometry.
// Stateless pure functions.
#pragma once

#include "pmi/pmi.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::pmi {

inline constexpr int WELD_DECIMALS = 3;        // weld.md §8
inline constexpr double WELD_CIRCLE_R = 0.5;   // "all around" circle radius, in units of the text height (§5, §8)

// Weld type table (§2.2), embedded from docs/dev/spec/formats/weld-types.json.
const Json& weldTypes();
// Standard by id; type of a standard; nullptr — none.
const Json* weldStandard(const std::string& id);
const Json* weldTypeInfo(const std::string& standard, const std::string& type);

// Weld record check against the table (§3): nullopt — valid, otherwise the name of the offending field.
std::optional<std::string> weldCheck(const Json& w);

// Label item (§4).
struct WeldLabelItem {
    enum class Kind { Text, Glyph };
    Kind kind = Kind::Text;
    std::string text;   // Text
    std::string glyph;  // Glyph: leg | removeReinforcement | smooth
    bool operator==(const WeldLabelItem&) const = default;
};
// Number texts of the label (§4): formatted values or the panel field texts.
struct WeldTexts {
    std::optional<std::string> leg, length, pitch;
};
std::vector<WeldLabelItem> weldLabel(const Json& w, const WeldTexts& texts);
// One-line label (§4): glyphs as ⊿, ⌒, ◡.
std::string weldLabelText(const std::vector<WeldLabelItem>& items);

// Label symbol (§5): origin — left edge on the text baseline, unit — the text height; advance — its width in the line.
struct WeldGlyph {
    Glyph glyph;
    double advance = 0;
};
WeldGlyph weldGlyph(const std::string& kind);
// Field weld flag at the leader break point (§5); withCircle — the staff starts on the "all around" circle.
Glyph weldFlagGlyph(bool withCircle);

}  // namespace construe::pmi
