// Common string helpers (base/strings.h, spec/base.md §7б).
#include "base/strings.h"

namespace construe {

std::string trim(const std::string& s, std::string_view chars) {
    const auto b = s.find_first_not_of(chars);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(chars);
    return s.substr(b, e - b + 1);
}

std::pair<int, int> lineColumn(const std::string& text, size_t pos) {
    int line = 1, col = 1;
    for (size_t i = 0; i < pos && i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            ++line;
            col = 1;
        } else if ((c & 0xC0) != 0x80) {
            ++col;
        }
    }
    return {line, col};
}

}  // namespace construe
