#include "pmi/pmi.h"

#include "base/number_format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>
#include <set>

namespace construe::pmi {

namespace {

const unsigned char kFitTables[] = {
#include "pmi/fit_tables_data.inc"
};

const std::set<std::string> kShaftLetters = {"a", "b", "c", "cd", "d", "e", "ef", "f", "fg", "g", "h", "js", "j", "k", "m",
                                             "n", "p", "r", "s", "t", "u", "v", "x", "y", "z", "za", "zb", "zc"};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int rangeIndex(const Json& bounds, double n) {
    for (size_t i = 0; i < bounds.size(); ++i)
        if (n <= bounds[i].get<double>()) return static_cast<int>(i);
    return -1;
}

// Table value, µm; null or absence — nullopt.
std::optional<double> cell(const Json& table, const std::string& key, int index) {
    if (!table.contains(key)) return std::nullopt;
    const Json& arr = table[key];
    if (index < 0 || static_cast<size_t>(index) >= arr.size() || arr[static_cast<size_t>(index)].is_null()) return std::nullopt;
    return arr[static_cast<size_t>(index)].get<double>();
}

struct CharInfo {
    std::string name;
    bool datums, diameter, material;
};

const std::map<std::string, CharInfo>& charTable() {
    static const std::map<std::string, CharInfo> t = {
        {"straightness", {"Прямолинейность", false, true, true}},
        {"flatness", {"Плоскостность", false, false, false}},
        {"roundness", {"Круглость", false, false, false}},
        {"cylindricity", {"Цилиндричность", false, false, false}},
        {"parallelism", {"Параллельность", true, true, true}},
        {"perpendicularity", {"Перпендикулярность", true, true, true}},
        {"angularity", {"Наклон", true, true, true}},
        {"position", {"Позиционный", true, true, true}},
        {"coaxiality", {"Соосность", true, true, true}},
        {"symmetry", {"Симметричность", true, false, true}},
        {"radialRunout", {"Радиальное биение", true, false, false}},
        {"axialRunout", {"Торцовое биение", true, false, false}},
        {"totalRadialRunout", {"Полное радиальное биение", true, false, false}},
    };
    return t;
}

// Code points of a UTF-8 string (the string is already valid — checked by JSON parsing).
std::vector<char32_t> codePoints(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
    return out;
}

}  // namespace

const Json& fitTables() {
    static const Json t = [] {
        const std::string text(reinterpret_cast<const char*>(kFitTables), sizeof(kFitTables));
        return Json::parse(text);
    }();
    return t;
}

// ---------------------------------------------------------------- §4.1 field parsing

Result<FieldParts> parseField(const std::string& field) {
    Error bad("PMI_FIELD_SYNTAX");
    bad.with("name", field);
    size_t i = 0;
    while (i < field.size() && std::isalpha(static_cast<unsigned char>(field[i]))) ++i;
    const std::string letters = field.substr(0, i), digits = field.substr(i);
    if (letters.empty() || digits.empty() || digits.size() > 2 || digits[0] == '0') return bad;
    for (char c : digits)
        if (!std::isdigit(static_cast<unsigned char>(c))) return bad;
    const bool allUpper = std::all_of(letters.begin(), letters.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
    const bool allLower = std::all_of(letters.begin(), letters.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
    if (!allUpper && !allLower) return bad;
    if (!kShaftLetters.count(lower(letters))) return bad;
    return FieldParts{letters, std::stoi(digits), allUpper};
}

// ---------------------------------------------------------------- §4.2 grade tolerance

Result<double> standardTolerance(int grade, double nominal) {
    const double n = std::round(nominal / FIT_NOMINAL_ROUND) * FIT_NOMINAL_ROUND;
    if (!(n > 0) || n > FIT_MAX_NOMINAL) return Error("PMI_SIZE_RANGE").with("actual", nominal);
    const Json& t = fitTables();
    const auto it = cell(t["it"], std::to_string(grade), rangeIndex(t["mainRanges"], n));
    if (!it) return Error("PMI_FIELD_UNDEFINED").with("name", "IT" + std::to_string(grade)).with("actual", nominal);
    return *it / 1000.0;
}

// ---------------------------------------------------------------- §4.3 limit deviations

Result<Deviations> fieldDeviations(const std::string& field, double nominal) {
    auto parts = parseField(field);
    if (!parts) return parts.error();
    const double n = std::round(nominal / FIT_NOMINAL_ROUND) * FIT_NOMINAL_ROUND;
    if (!(n > 0) || n > FIT_MAX_NOMINAL) return Error("PMI_SIZE_RANGE").with("actual", nominal);
    const Json& t = fitTables();
    const int mi = rangeIndex(t["mainRanges"], n), di = rangeIndex(t["detailedRanges"], n);
    Error undefined("PMI_FIELD_UNDEFINED");
    undefined.with("name", field).with("actual", nominal);
    const int g = parts->grade;
    const std::string gs = std::to_string(g), l = lower(parts->letters);
    // Not applied for nominal sizes up to 1 mm (§4.3; GOST 25346-2013 tables 1–4, footnotes): a, b (A, B); N above IT8;
    // grade 14 and coarser.
    if (n <= FIT_SMALL_NOMINAL && (l == "a" || l == "b" || g >= 14 || (parts->hole && l == "n" && g > 8))) return undefined;
    const auto it = cell(t["it"], gs, mi);
    if (!it) return undefined;
    const double T = *it;
    const auto delta = [&]() { return cell(t["delta"], gs, mi); };
    // Lower deviation of the shaft letter l (k, m…zc), µm.
    const auto shaftEi = [&](const std::string& letter) -> std::optional<double> {
        if (letter == "k") return (g >= 4 && g <= 7) ? cell(t["shaftEiMain"], "k", mi) : std::optional<double>(0.0);
        if (letter == "m" || letter == "n" || letter == "p") return cell(t["shaftEiMain"], letter, mi);
        return cell(t["shaftEiDetailed"], letter, di);
    };
    double up = 0, lo = 0;
    if (!parts->hole) {
        if (t["shaftEs"].contains(l)) {
            const auto es = cell(t["shaftEs"], l, di);
            if (!es) return undefined;
            up = *es, lo = *es - T;
        } else if (l == "js") {
            up = T / 2, lo = -T / 2;
        } else if (l == "j") {
            const auto ei = cell(t["shaftEiJ"], gs, mi);
            if (!ei) return undefined;
            lo = *ei, up = *ei + T;
        } else {
            const auto ei = shaftEi(l);
            if (!ei) return undefined;
            lo = *ei, up = *ei + T;
        }
    } else {
        if (t["shaftEs"].contains(l)) {  // A…H
            const auto es = cell(t["shaftEs"], l, di);
            if (!es) return undefined;
            lo = -*es, up = lo + T;
        } else if (l == "js") {
            up = T / 2, lo = -T / 2;
        } else {
            double ES = 0;
            if (l == "j") {
                const auto es = cell(t["holeEsJ"], gs, mi);
                if (!es) return undefined;
                ES = *es;
            } else if (l == "k" || l == "m" || l == "n") {
                if (mi == 0) {
                    ES = l == "k" ? 0 : l == "m" ? -2 : -4;
                } else {
                    const auto ei = cell(t["shaftEiMain"], l, mi);
                    if (!ei) return undefined;
                    if (g <= 8) {
                        const auto d = delta();
                        if (!d) return undefined;
                        ES = -*ei + *d;
                    } else {
                        if (l == "k") return undefined;  // K above IT8 over 3 mm is not defined (GOST 25347-2013 table 8, b)
                        ES = l == "m" ? -*ei : 0;
                    }
                }
            } else {  // P…ZC
                const auto ei = shaftEi(l);
                if (!ei) return undefined;
                if (g <= 7) {
                    const auto d = delta();
                    if (!d) return undefined;
                    ES = -*ei + *d;
                } else {
                    ES = -*ei;
                }
            }
            up = ES, lo = ES - T;
        }
    }
    for (const auto& e : t["exceptions"])
        if (e["field"].get<std::string>() == field && n > e["above"].get<double>() && n <= e["upTo"].get<double>()) {
            up = e["upper"].get<double>();
            lo = up - T;
        }
    return Deviations{up / 1000.0, lo / 1000.0};
}

// ---------------------------------------------------------------- §2 designation rules

const std::vector<std::string>& characteristics() {
    static const std::vector<std::string> v = {"straightness", "flatness", "roundness", "cylindricity", "parallelism",
                                               "perpendicularity", "angularity", "position", "coaxiality", "symmetry",
                                               "radialRunout", "axialRunout", "totalRadialRunout"};
    return v;
}

bool characteristicKnown(const std::string& c) { return charTable().count(c) > 0; }
bool needsDatums(const std::string& c) {
    const auto it = charTable().find(c);
    return it != charTable().end() && it->second.datums;
}
bool allowsDatums(const std::string& c) { return needsDatums(c); }
bool allowsDiameter(const std::string& c) {
    const auto it = charTable().find(c);
    return it != charTable().end() && it->second.diameter;
}
bool allowsMaterial(const std::string& c) {
    const auto it = charTable().find(c);
    return it != charTable().end() && it->second.material;
}
std::string characteristicName(const std::string& c) {
    const auto it = charTable().find(c);
    return it != charTable().end() ? it->second.name : c;
}

bool toleranceDimension(const std::string& t) {
    return sizeDimension(t) || t == "radius" || t == "arcLength" || t == "pathLength";
}
bool sizeDimension(const std::string& t) {
    return t == "distance" || t == "distanceX" || t == "distanceY" || t == "length" || t == "diameter";
}

bool validDatumLetter(const std::string& s) {
    const auto cps = codePoints(s);
    if (cps.empty() || cps.size() > 2) return false;
    for (char32_t c : cps)
        if (!((c >= U'A' && c <= U'Z') || (c >= 0x0410 && c <= 0x042F) || c == 0x0401)) return false;
    return true;
}

const std::vector<std::string>& designationLetters() {
    static const std::vector<std::string> v = {"А", "Б", "В", "Г", "Д", "Е", "Ж", "И", "К", "Л", "М", "Н",
                                               "П", "Р", "С", "Т", "У", "Ф", "Ц", "Ш", "Щ", "Э", "Ю", "Я"};
    return v;
}

std::string nextDatumLetter(const std::vector<std::string>& used) {
    for (const auto& l : designationLetters())
        if (std::find(used.begin(), used.end(), l) == used.end()) return l;
    return "А";
}

std::string laySymbol(const std::string& lay) {
    static const std::map<std::string, std::string> m = {{"parallel", "="}, {"perpendicular", "⊥"}, {"crossed", "X"}, {"multi", "M"},
                                                         {"circular", "C"}, {"radial", "R"}, {"particulate", "P"}};
    const auto it = m.find(lay);
    return it != m.end() ? it->second : std::string();
}

// ---------------------------------------------------------------- §5 texts

std::string formatNumber(double v, int decimals) { return localizeNumber(fixedTrimmed(v, decimals)); }

std::string formatDeviation(double v) {
    const std::string body = formatNumber(std::abs(v), DEV_DECIMALS);
    if (body == "0") return "0";
    return (v > 0 ? "+" : "−") + body;
}

std::string deviationsText(const Deviations& d) {
    if (std::abs(d.upper + d.lower) <= 1e-9 && std::abs(d.upper) > 1e-9) return "±" + formatNumber(std::abs(d.upper), DEV_DECIMALS);
    const std::string u = formatDeviation(d.upper), l = formatDeviation(d.lower);
    if (u != "0" && l != "0") return u + "/" + l;
    if (u != "0") return u;
    if (l != "0") return l;
    return "0";
}

std::string toleranceSuffix(const Json& fit, const std::optional<Deviations>& d) {
    if (fit.contains("field")) {
        std::string s = fit["field"].get<std::string>();
        if (fit.value("showDeviations", false) && d) s += "(" + deviationsText(*d) + ")";
        return s;
    }
    if (fit.contains("upper") && fit.contains("lower"))
        return " " + deviationsText({fit["upper"].get<double>(), fit["lower"].get<double>()});
    return {};
}

std::string toleranceLabel(const Json& fit, const std::optional<Deviations>& d) {
    if (fit.contains("field")) {
        std::string s = fit["field"].get<std::string>();
        if (d) s += "(" + deviationsText(*d) + ")";
        return s;
    }
    return toleranceSuffix(fit, d);
}

std::string roughnessText(const Json& a) {
    if (!a.contains("parameter") || !a.contains("value")) return {};
    return a["parameter"].get<std::string>() + " " + formatNumber(a["value"].get<double>(), ROUGHNESS_DECIMALS);
}

std::string frameValueText(const Json& a) {
    std::string s = a.value("diameter", false) ? "⌀" : "";
    if (a.contains("value")) s += formatNumber(a["value"].get<double>(), DEV_DECIMALS);
    return s;
}

// ---------------------------------------------------------------- §6 symbol geometry

Glyph characteristicGlyph(const std::string& c) {
    Glyph g;
    auto line = [&](double x0, double y0, double x1, double y1) { g.polylines.push_back({Point2(x0, y0), Point2(x1, y1)}); };
    auto circle = [&](double x, double y, double r) { g.circles.push_back({Point2(x, y), r}); };
    auto head = [&](double x0, double y0, double x1, double y1, double x2, double y2) {
        g.filled.push_back({Point2(x0, y0), Point2(x1, y1), Point2(x2, y2)});
    };
    if (c == "straightness") line(0.15, 0.5, 0.85, 0.5);
    else if (c == "flatness")
        g.polylines.push_back({Point2(0.1, 0.3), Point2(0.7, 0.3), Point2(0.9, 0.7), Point2(0.3, 0.7), Point2(0.1, 0.3)});
    else if (c == "roundness") circle(0.5, 0.5, 0.32);
    else if (c == "cylindricity") {
        circle(0.5, 0.5, 0.25);
        line(0.15, 0.15, 0.45, 0.85), line(0.55, 0.15, 0.85, 0.85);
    } else if (c == "parallelism") {
        line(0.25, 0.15, 0.5, 0.85), line(0.5, 0.15, 0.75, 0.85);
    } else if (c == "perpendicularity") {
        line(0.5, 0.2, 0.5, 0.8), line(0.15, 0.2, 0.85, 0.2);
    } else if (c == "angularity") {
        line(0.15, 0.2, 0.85, 0.2), line(0.15, 0.2, 0.75, 0.7);
    } else if (c == "position") {
        circle(0.5, 0.5, 0.28);
        line(0.5, 0.1, 0.5, 0.9), line(0.1, 0.5, 0.9, 0.5);
    } else if (c == "coaxiality") {
        circle(0.5, 0.5, 0.18), circle(0.5, 0.5, 0.35);
    } else if (c == "symmetry") {
        line(0.1, 0.5, 0.9, 0.5), line(0.25, 0.3, 0.75, 0.3), line(0.25, 0.7, 0.75, 0.7);
    } else if (c == "radialRunout" || c == "axialRunout") {
        line(0.25, 0.15, 0.75, 0.85);
        head(0.75, 0.85, 0.58, 0.78, 0.7, 0.64);
    } else if (c == "totalRadialRunout") {
        line(0.15, 0.15, 0.55, 0.85), line(0.45, 0.15, 0.85, 0.85), line(0.15, 0.15, 0.45, 0.15);
        head(0.55, 0.85, 0.38, 0.78, 0.5, 0.64);
        head(0.85, 0.85, 0.68, 0.78, 0.8, 0.64);
    }
    return g;
}

Glyph roughnessGlyph(const std::string& sign, double textWidth, bool shelf) {
    Glyph g;
    const double h = ROUGHNESS_SHORT_H, H = ROUGHNESS_LONG_H, xl = ROUGHNESS_SHORT_LEG_X, xr = ROUGHNESS_LONG_LEG_X;
    g.polylines.push_back({Point2(xl, h), Point2(0, 0), Point2(xr, H)});
    // Removal: the bar closes the equilateral triangle of height h; no removal: its inscribed circle (r = h/3).
    if (sign == "removal") g.polylines.push_back({Point2(xl, h), Point2(-xl, h)});
    if (sign == "noRemoval") g.circles.push_back({Point2(0, 0.667 * h), 0.333 * h});
    if (shelf) g.polylines.push_back({Point2(xr, H), Point2(xr + textWidth + ROUGHNESS_SHELF_PAD, H)});
    return g;
}

// ---------------------------------------------------------------- §5.4 tolerance in the dimension field

Result<std::optional<Json>> parseToleranceText(const std::string& text) {
    Error bad("PMI_FIELD_SYNTAX");
    bad.with("name", text);
    // Normalization: spaces removed, U+2212 → '-', decimal comma → point, "+-" / "-+" → '±'.
    std::string t;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (text.compare(i, 3, "\xE2\x88\x92") == 0) t += '-', i += 3;            // U+2212 minus
        else if (text.compare(i, 2, "\xC2\xB1") == 0) t += '~', i += 2;           // U+00B1 plus-minus
        else if (std::isspace(c)) ++i;
        else t += c == ',' ? '.' : char(c), ++i;
    }
    if (t.empty()) return std::optional<Json>();
    if (t.rfind("+-", 0) == 0 || t.rfind("-+", 0) == 0) t = "~" + t.substr(2);
    static const std::regex num(R"(^([+\-~]?)(\d+(\.\d*)?|\.\d+)$)");
    auto number = [&](const std::string& s, char& sign, double& v) {
        std::smatch m;
        if (!std::regex_match(s, m, num)) return false;
        sign = m[1].length() ? m[1].str()[0] : 0;
        v = std::stod(m[2].str());
        return true;
    };
    // Zone: letters + grade, optionally followed by the deviations in parentheses (shown with the zone).
    {
        static const std::regex zone(R"(^([A-Za-z]{1,2}\d{1,2})(\(.*\))?$)");
        std::smatch m;
        if (std::regex_match(t, m, zone)) {
            if (!parseField(m[1].str())) return bad;
            Json j = {{"field", m[1].str()}};
            if (m[2].length()) j["showDeviations"] = true;
            return std::optional<Json>(j);
        }
    }
    char s1 = 0, s2 = 0;
    double v1 = 0, v2 = 0;
    const size_t slash = t.find('/');
    if (slash == std::string::npos) {
        if (!number(t, s1, v1) || !(v1 > 0)) return bad;
        Json j;
        if (s1 == '+') j = {{"upper", v1}, {"lower", 0.0}};
        else if (s1 == '-') j = {{"upper", 0.0}, {"lower", -v1}};
        else j = {{"upper", v1}, {"lower", -v1}};  // "±v" and an unsigned number — symmetric
        return std::optional<Json>(j);
    }
    if (!number(t.substr(0, slash), s1, v1) || !number(t.substr(slash + 1), s2, v2) || s1 == '~' || s2 == '~') return bad;
    const double up = s1 == '-' ? -v1 : v1, lo = s2 == '-' ? -v2 : v2;
    if (!(up > lo)) return bad;
    return std::optional<Json>(Json{{"upper", up}, {"lower", lo}});
}

}  // namespace construe::pmi
