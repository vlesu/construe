// Hole table for metric fasteners (spec/regen.md §4.13, formats/hole-tables.json).
#pragma once

#include "doc/json_text.h"

#include <optional>
#include <string>

namespace construe::regen {

// Built-in table (parsed JSON of the construe-hole-tables format).
const doc::Json& holeTables();
// Size row ("M6"); nullopt — not in the table.
std::optional<doc::Json> holeSize(const std::string& size);
// Thread table (formats/thread-tables.json, S2-FEAT-009) and designation row.
const doc::Json& threadTables();
// Material library (formats/materials.json, materials.md, S2-PART-002).
const doc::Json& materialLibrary();
std::optional<doc::Json> threadSpec(const std::string& designation);
// Thread given by expressions (regen.md §4.14 item 2, S6-LIB-008): the coarse pitch of the metric thread of nominal d —
// pitch of the table row "M<d>" (d rounded to 1e-6 mm, canonical notation); nullopt — no such row.
std::optional<double> threadCoarsePitch(double d);
// Designation of a metric thread of nominal d and pitch P: "M<d>" for the coarse pitch, otherwise "M<d>x<P>".
std::string threadDesignation(double d, double P);

// Whether a cylindrical face of diameter D fits a thread with nominal d and pitch P (regen.md §4.14 item 3, FX2-FEAT-001):
// external — by nominal; internal — by tap drill diameter (TapDrill) or by nominal (Nominal).
enum class ThreadDiameterFit { None, TapDrill, Nominal };
ThreadDiameterFit threadDiameterFit(double d, double P, double D, bool internal);
// Allowed diameters (mm): tap drill [tapMin, tapMax], nominal [nomMin, nomMax] (§4.14 item 3).
struct ThreadDiameterRanges {
    double tapMin = 0, tapMax = 0, nomMin = 0, nomMax = 0;
};
ThreadDiameterRanges threadDiameterRanges(double d, double P);
// Tap drill diameter (mm): from the hole table (row "M<d>", pitch P — drill), otherwise d − P rounded to 0.1.
double threadTapDrill(double d, double P);

}  // namespace construe::regen
