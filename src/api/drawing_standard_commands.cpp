// Session: drawing standard and projection method (spec/core-api.md §4.1г, §5; drawing.md §2.5, §3.2, §6.2; S3-DRW-008)
#include "api/errors.h"
#include "api/session.h"

#include "drawing/cut_plan.h"
#include "drawing/standard.h"

namespace construe::api {

Status Session::updateDrawing(const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    const doc::Element& el = doc().active();
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const std::string id = el.id;
    if (!def.is_object()) return invalidDefinition("definition must be an object");
    for (auto it = def.begin(); it != def.end(); ++it)
        if (it.key() != "standard" && it.key() != "projection") return invalidDefinition(it.key() + " not allowed", "/" + it.key());
    // Merge with the element (core-api §4.1г): null removes the field, else replaces it; validated by document schema.
    Json dj = doc::documentToJsonValue(doc());
    const int ei = doc().activeIndex();
    Json& ej = dj["elements"][size_t(ei)];
    mergePatch(ej, def);
    auto r = doc::documentFromJson(dj);
    if (!r) return schemaErrorAt(r.error(), "/elements/" + std::to_string(ei));
    r->activeElement = doc().activeElement;  // documentFromJson makes the first element active
    // Effective projection method changed - projected views move to the other side of their parents (drawing.md §6.2).
    doc::Element& ne = r->elements[size_t(ei)];
    if (drawing::projectionMethod(ne) != drawing::projectionMethod(el)) drawing::mirrorProjectedViews(ne);
    return commit(std::move(*r), "undo.updateDrawing", {}, id);
}

void Session::setCutLabelFormat(const std::string& withMaterial, const std::string& withoutMaterial, const std::string& decimalSeparator) {
    drawing::setCutLabelFormat(withMaterial, withoutMaterial, decimalSeparator);
}

void Session::setPreferredDrawingFont(font::DrawingStandard s, const std::string& name) { drawing::setPreferredDrawingFont(s, name); }

}  // namespace construe::api
