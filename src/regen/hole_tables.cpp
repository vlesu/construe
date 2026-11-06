#include "regen/hole_tables.h"

#include "base/number_format.h"

#include <cmath>
#include <string>

namespace construe::regen {

namespace {

const unsigned char kHoleTablesData[] = {
#include "regen/hole_tables_data.inc"
};
const unsigned char kThreadTablesData[] = {
#include "regen/thread_tables_data.inc"
};
const unsigned char kMaterialsData[] = {
#include "regen/materials_data.inc"
};

}  // namespace

const doc::Json& holeTables() {
    static const doc::Json t = doc::Json::parse(std::string(reinterpret_cast<const char*>(kHoleTablesData), sizeof(kHoleTablesData)));
    return t;
}

const doc::Json& materialLibrary() {
    static const doc::Json t = doc::Json::parse(std::string(reinterpret_cast<const char*>(kMaterialsData), sizeof(kMaterialsData)));
    return t;
}

const doc::Json& threadTables() {
    static const doc::Json t = doc::Json::parse(std::string(reinterpret_cast<const char*>(kThreadTablesData), sizeof(kThreadTablesData)));
    return t;
}

std::optional<doc::Json> threadSpec(const std::string& designation) {
    for (const auto& row : threadTables()["threads"])
        if (row["designation"] == designation) return std::optional<doc::Json>(row);
    return std::nullopt;
}

namespace {
// Number of a designation: rounded to 1e-6 mm, canonical notation.
std::string designationNumber(double v) { return canonicalNumber(std::round(v * 1e6) / 1e6); }
}  // namespace

std::optional<double> threadCoarsePitch(double d) {
    const auto row = threadSpec("M" + designationNumber(d));
    if (!row) return std::nullopt;
    return (*row)["pitch"].get<double>();
}

std::string threadDesignation(double d, double P) {
    const std::string base = "M" + designationNumber(d);
    const auto coarse = threadCoarsePitch(d);
    if (coarse && std::abs(*coarse - P) <= 1e-9) return base;
    return base + "x" + designationNumber(P);
}

ThreadDiameterRanges threadDiameterRanges(double d, double P) {
    return {d - 1.2 * P, d - 0.8 * P, d - 0.25 * P, d + 0.01};
}

ThreadDiameterFit threadDiameterFit(double d, double P, double D, bool internal) {
    const ThreadDiameterRanges r = threadDiameterRanges(d, P);
    if (internal && D >= r.tapMin - 1e-9 && D <= r.tapMax + 1e-9) return ThreadDiameterFit::TapDrill;
    if (D >= r.nomMin - 1e-9 && D <= r.nomMax) return ThreadDiameterFit::Nominal;
    return ThreadDiameterFit::None;
}

double threadTapDrill(double d, double P) {
    for (const auto& row : holeTables()["sizes"]) {
        const std::string size = row["size"];
        if (size.size() < 2 || size[0] != 'M' || std::abs(std::stod(size.substr(1)) - d) > 1e-9) continue;
        for (const auto& p : row["pitches"])
            if (std::abs(p["pitch"].get<double>() - P) <= 1e-9) return p["drill"].get<double>();
    }
    return std::round((d - P) * 10) / 10;
}

std::optional<doc::Json> holeSize(const std::string& size) {
    for (const auto& row : holeTables()["sizes"])
        if (row["size"] == size) return std::optional<doc::Json>(row);
    return std::nullopt;
}

}  // namespace construe::regen
