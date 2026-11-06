// Sheet marks (spec/drawing.md §6.25, FX9-DRW-008, FX9-DRW-004).
#include "drawing/sheet_marks.h"

#include <algorithm>

namespace construe::drawing {

std::string markModel(const doc::Sheet& sheet, const doc::SheetMark& m) {
    const auto t = viewTriad(sheet, m.view);
    if (!t || !t->base->refFile.empty()) return {};
    return t->base->refElement;
}

doc::DismissedMark markKey(const doc::Sheet& sheet, const doc::SheetMark& m) { return {m.kind, markModel(sheet, m), m.source}; }

std::vector<doc::SheetMark> modelMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                       const ModelProvider& models, const std::vector<std::string>& views) {
    // Keys actually on the drawing (any sheet; this sheet — as passed). A mark deleted by the user is placed again
    // (FX12-DRW-005): the obsolete dismissedMarks of old files are ignored.
    std::vector<doc::DismissedMark> taken;
    bool sheetSeen = false;
    for (const auto& sh : drawing.sheets) {
        const doc::Sheet& s = sh.id == sheet.id ? sheet : sh;
        sheetSeen = sheetSeen || sh.id == sheet.id;
        for (const auto& m : s.marks) taken.push_back(markKey(s, m));
    }
    if (!sheetSeen)
        for (const auto& m : sheet.marks) taken.push_back(markKey(sheet, m));
    std::vector<doc::SheetMark> out;
    auto add = [&](std::vector<doc::SheetMark> marks) {
        for (auto& m : marks) {
            const doc::DismissedMark k = markKey(sheet, m);
            if (std::find(taken.begin(), taken.end(), k) != taken.end()) continue;
            taken.push_back(k);
            out.push_back(std::move(m));
        }
    };
    add(annotationMarks(d, drawing, sheet, models, views));
    add(weldMarks(d, drawing, sheet, models));
    return out;
}

std::optional<std::pair<Point2, size_t>> nearestOnPolylines(const std::vector<std::vector<Point2>>& polys, const Point2& p) {
    std::optional<std::pair<Point2, size_t>> best;
    double bestD = 0;
    for (size_t i = 0; i < polys.size(); ++i) {
        const auto& pl = polys[i];
        for (size_t j = 0; j < pl.size(); ++j) {
            Point2 q = pl[j];
            if (j + 1 < pl.size()) {
                const Point2 a = pl[j], dv = pl[j + 1] - a;
                const double l2 = dv.squaredNorm();
                const double t = l2 > 0 ? std::clamp((p - a).dot(dv) / l2, 0.0, 1.0) : 0.0;
                q = a + t * dv;
            } else if (pl.size() > 1) {
                continue;  // the last vertex is the end of the previous segment
            }
            const double dd = (p - q).norm();
            if (!best || dd < bestD) {
                best = std::make_pair(q, i);
                bestD = dd;
            }
        }
    }
    return best;
}

}  // namespace construe::drawing
