// GOST 2.104 frame and title block (spec/drawing.md §5): field keys and default values.
#pragma once
#include "drawing/sheet.h"

#include <map>
#include <string>
#include <vector>

namespace construe::drawing {

// Title block field keys in the order of the §5.4 table.
const std::vector<std::string>& titleBlockKeys();
// Sheet field values without the sheet's own titleBlock: inheritance from the first sheet and default values (§5.6).
std::map<std::string, std::string> titleBlockDefaults(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                                      const ModelProvider& models);
// Mass, kg, as the text of field 5 (§5.6); m ≤ 0 — empty.
std::string formatMass(double kg);

// A calendar date (FX14-DRW-008, §5.4а).
struct CalendarDate {
    int year = 1970, month = 1, day = 1;
};
// Today's local date of the computer.
CalendarDate today();
// A field value with date templates `{now:<format>}` replaced by the date (§5.4а): in the format d — day, two digits;
// m — month, two digits; Y — year, four digits; y — year, two digits; any other character — as is. Text outside the
// templates, an unclosed «{now:» and other braces — unchanged.
std::string expandDateTemplates(const std::string& value, const CalendarDate& date);
// True if the value holds a date template (§5.4а).
bool hasDateTemplate(const std::string& value);

// Title block form: 1 — first sheet of a drawing (§5.3), 2 — first sheet of a text document (§5.3а), 2а —
// subsequent sheets (§5.3).
enum class TitleForm { Form1, Form2, Form2a };
// Frame, lines and form texts, additional fields of a sheet W × H (§5.1–§5.5) by field values values (key → text,
// missing key — empty); graph26 — draw field 26 (§5.3а).
void appendTitleFrame(double W, double H, TitleForm form, const std::map<std::string, std::string>& values, bool graph26,
                      SheetGraphics& g);
// Format notation of the sheet format for field 30 and «Format» (§5.6): «А4×3».
std::string formatLabel(const std::string& format);

}  // namespace construe::drawing
