// Locale-independent parsing of floating-point numbers (base/charconv.h).
#include "base/charconv.h"

#include <cerrno>
#include <cmath>
#include <charconv>
#include <clocale>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <locale.h>
#elif defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif

namespace construe {

namespace {

#if defined(_WIN32)
using CLocale = _locale_t;
CLocale cLocale() {
    static const CLocale l = _create_locale(LC_ALL, "C");
    return l;
}
double strtodC(const char* s, char** end) { return _strtod_l(s, end, cLocale()); }
#else
using CLocale = locale_t;
CLocale cLocale() {
    static const CLocale l = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return l;
}
double strtodC(const char* s, char** end) { return strtod_l(s, end, cLocale()); }
#endif

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The value of an out-of-range number, the same on every platform (spec/base.md §2б): ±inf on overflow, ±0 on
// underflow. std::from_chars leaves `value` unchanged in libstdc++ and writes ±inf/0 in MSVC, so neither is used.
double outOfRangeValue(bool negative, bool overflow) {
    const double v = overflow ? HUGE_VAL : 0.0;
    return negative ? -v : v;
}

}  // namespace

ParseDoubleResult parseDoubleStrtod(const char* first, const char* last, double& value) {
    // The longest prefix with the from_chars syntax: [-](digits[.digits]|.digits)[(e|E)[+|-]digits] | [-]inf[inity] |
    // [-]nan[(chars)] — strtod alone would also take spaces, '+' and hexadecimal.
    const char* p = first;
    if (p < last && *p == '-') ++p;
    const char* body = p;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    auto word = [&](const char* w) {
        const char* q = p;
        for (; *w; ++w, ++q)
            if (q >= last || lower(*q) != *w) return static_cast<const char*>(nullptr);
        return q;
    };
    const char* end = nullptr;
    if (const char* q = word("inf")) {
        p = q;
        if (const char* r = word("inity")) p = r;
        end = p;
    } else if (const char* q2 = word("nan")) {
        p = q2;
        end = p;
        if (p < last && *p == '(') {
            const char* r = p + 1;
            while (r < last && (isDigit(*r) || (lower(*r) >= 'a' && lower(*r) <= 'z') || *r == '_')) ++r;
            if (r < last && *r == ')') end = r + 1;
        }
    } else {
        bool digits = false;
        while (p < last && isDigit(*p)) ++p, digits = true;
        if (p < last && *p == '.') {
            ++p;
            while (p < last && isDigit(*p)) ++p, digits = true;
        }
        if (!digits) return {first, std::errc::invalid_argument};
        end = p;
        if (p < last && (*p == 'e' || *p == 'E')) {
            const char* q = p + 1;
            if (q < last && (*q == '+' || *q == '-')) ++q;
            if (q < last && isDigit(*q)) {
                while (q < last && isDigit(*q)) ++q;
                end = q;
            }
        }
    }
    if (end == body) return {first, std::errc::invalid_argument};
    const std::string text(first, end);
    char* stop = nullptr;
    errno = 0;
    const double v = strtodC(text.c_str(), &stop);
    if (stop != text.c_str() + text.size()) return {first, std::errc::invalid_argument};
    // Overflow and underflow to zero — out of range; a subnormal result is a value.
    if (errno == ERANGE && (v == 0.0 || v > 1.0 || v < -1.0)) {
        value = outOfRangeValue(*first == '-', v > 1.0 || v < -1.0);
        return {end, std::errc::result_out_of_range};
    }
    value = v;
    return {end, std::errc()};
}

ParseDoubleResult parseDouble(const char* first, const char* last, double& value) {
#if defined(_LIBCPP_VERSION) && !(defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L)
    return parseDoubleStrtod(first, last, value);
#else
    double v = 0.0;
    const auto r = std::from_chars(first, last, v);
    if (r.ec == std::errc()) {
        value = v;
    } else if (r.ec == std::errc::result_out_of_range) {
        // Overflow or underflow is told by strtod over the same prefix (huge or not); the value itself is normalized.
        const std::string text(first, r.ptr);
        const double h = strtodC(text.c_str(), nullptr);
        value = outOfRangeValue(*first == '-', h > 1.0 || h < -1.0);
    }
    return {r.ptr, r.ec};
#endif
}

}  // namespace construe
