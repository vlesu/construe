#include "base/number_format.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace construe {

std::string canonicalNumber(double value) {
    assert(std::isfinite(value));
    if (value == 0.0) return "0";  // including -0

    // Shortest significant digits and exponent via scientific to_chars without precision.
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), std::abs(value), std::chars_format::scientific);
    assert(res.ec == std::errc());
    std::string_view s(buf, static_cast<size_t>(res.ptr - buf));  // of the form "d.ddde+XX" or "de-XX"

    const size_t ePos = s.find('e');
    std::string digits;
    for (char c : s.substr(0, ePos))
        if (c != '.') digits.push_back(c);
    const int exp10 = std::atoi(std::string(s.substr(ePos + 1)).c_str());

    // ECMAScript: k — number of digits, n — position of the decimal point (value = 0.digits × 10^n).
    const int k = static_cast<int>(digits.size());
    const int n = exp10 + 1;

    std::string out;
    if (value < 0) out.push_back('-');

    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, static_cast<size_t>(n));
        out.push_back('.');
        out += digits.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<size_t>(-n), '0');
        out += digits;
    } else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out += digits.substr(1);
        }
        out.push_back('e');
        const int e = n - 1;
        out.push_back(e < 0 ? '-' : '+');
        out += std::to_string(std::abs(e));
    }
    return out;
}

}  // namespace construe

namespace construe {

void trimFractionZeros(std::string& s) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return;
    size_t end = s.size();
    while (end > dot + 1 && s[end - 1] == '0') --end;
    if (end == dot + 1) end = dot;
    s.resize(end);
}

std::string fixedTrimmed(double v, int digits) {
    char buf[512];  // the largest double in fixed notation is 309 integer digits
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, digits);
    std::string s(buf, r.ec == std::errc() ? r.ptr : buf);
    trimFractionZeros(s);
    if (s == "-0") s = "0";
    return s;
}

std::string roundTrip(double v) {
    if (v == 0) return "0";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

std::string decimalComma(std::string s) {
    for (char& c : s)
        if (c == '.') c = ',';
    return s;
}

std::string localizeNumber(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '-') out += "\xE2\x88\x92";  // U+2212 minus sign
        else if (c == '.') out += ',';
        else out += c;
    }
    return out;
}

}  // namespace construe
