#include "base/ids.h"

#include <cassert>

namespace construe {

namespace {

struct Split {
    std::string_view prefix;
    std::string_view number;
    std::string_view piece;  // piece number after "@" (empty — none)
};

bool digits(std::string_view num) {
    if (num.empty()) return false;
    for (char c : num)
        if (c < '0' || c > '9') return false;
    return !(num.size() > 1 && num[0] == '0');
}

bool split(std::string_view id, Split& out, bool allowPiece) {
    std::string_view piece;
    if (const size_t at = id.find('@'); at != std::string_view::npos) {
        if (!allowPiece) return false;
        piece = id.substr(at + 1);
        if (!digits(piece) || piece == "0") return false;
        id = id.substr(0, at);
    }
    size_t i = 0;
    while (i < id.size() && id[i] >= 'a' && id[i] <= 'z') ++i;
    if (i == 0 || i == id.size()) return false;
    std::string_view num = id.substr(i);
    for (char c : num)
        if (c < '0' || c > '9') return false;
    if (num.size() > 1 && num[0] == '0') return false;
    out = {id.substr(0, i), num, piece};
    return true;
}

int compareNumbers(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

}  // namespace

bool isNumberedId(std::string_view id) {
    Split s;
    return split(id, s, false);
}

bool isSegmentId(std::string_view id) {
    Split s;
    return split(id, s, true);
}

int compareIds(std::string_view a, std::string_view b) {
    Split sa, sb;
    const bool okA = split(a, sa, true);
    const bool okB = split(b, sb, true);
    assert(okA && okB && "compareIds: not a numbered id");
    if (!okA || !okB) return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);

    if (int c = sa.prefix.compare(sb.prefix); c != 0) return c < 0 ? -1 : 1;
    if (int c = compareNumbers(sa.number, sb.number); c != 0) return c;
    if (sa.piece.empty() != sb.piece.empty()) return sa.piece.empty() ? -1 : 1;
    return compareNumbers(sa.piece, sb.piece);
}

}  // namespace construe
