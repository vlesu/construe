// Value display (spec/expr.md §6.1).
#include "expr/expr.h"

#include "base/number_format.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace construe::expr {

namespace {

std::string formatNumber(double v, int precision) {
    char buf[512];
    const double a = std::abs(v);
    if (v == 0.0 || (a >= 1e-6 && a < 1e15)) return fixedTrimmed(v, precision);
    auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific, precision);
    std::string s(buf, r.ptr);  // "d.ddde+XX"
    const size_t e = s.find('e');
    std::string mant = s.substr(0, e);
    trimFractionZeros(mant);
    const int ex = std::atoi(s.c_str() + e + 1);
    return mant + "e" + (ex < 0 ? "-" : "+") + std::to_string(std::abs(ex));
}

std::string unitPower(const char* unit, int k) {
    std::string s = unit;
    if (k != 1) s += "^" + std::to_string(k);
    return s;
}

}  // namespace

std::string format(const Quantity& q, const DocumentUnits& units, int precision) {
    const Dim d = q.dim;
    if (d.dimensionless()) return formatNumber(q.value, precision);
    if (d == LENGTH)
        return formatNumber(q.value / lengthFactor(units.length), precision) + " " + lengthUnitName(units.length);
    if (d == ANGLE) {
        if (units.angle == AngleUnit::deg) return formatNumber(q.value / (std::numbers::pi / 180.0), precision) + "°";
        return formatNumber(q.value, precision) + " rad";
    }
    const double f = std::pow(lengthFactor(units.length), d.L) * std::pow(angleFactor(units.angle), d.A);
    std::string suffix;
    if (d.L != 0) suffix += unitPower(lengthUnitName(units.length), d.L);
    if (d.A != 0) {
        if (!suffix.empty()) suffix += "*";
        suffix += unitPower(angleUnitName(units.angle), d.A);
    }
    return formatNumber(q.value / f, precision) + " " + suffix;
}

std::string formatPlain(const Quantity& q, const DocumentUnits& units, int precision) {
    if (q.dim == LENGTH) return formatNumber(q.value / lengthFactor(units.length), precision);
    if (q.dim == ANGLE) return formatNumber(q.value / angleFactor(units.angle), precision);
    return formatNumber(q.value, precision);
}

std::string substitute(const std::string& t, const VariableTable& table, const DocumentUnits& units) {
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if ((c == '{' || c == '}') && i + 1 < t.size() && t[i + 1] == c) {  // {{ and }}
            out += c;
            i += 2;
            continue;
        }
        if (c == '{') {
            const size_t close = t.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string name = t.substr(i + 1, close - i - 1);
                if (const VariableEntry* e = table.find(name)) {
                    if (e->isString()) {
                        out += e->text;
                        i = close + 1;
                        continue;
                    }
                    if (e->value.ok()) {
                        out += formatPlain(*e->value, units, SUBST_PRECISION);
                        i = close + 1;
                        continue;
                    }
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

}  // namespace construe::expr
