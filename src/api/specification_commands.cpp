// Session: specifications (BOMs) - editing, rows, virtual sheets (spec/core-api.md §4.1е, §5; spec/specification.md).
#include "api/errors.h"
#include "api/session.h"

namespace construe::api {

namespace {

Error notSpecification(const doc::Element& el) { return Error("API_NOT_SPECIFICATION").with("id", el.id); }

}  // namespace

drawing::LinkedDocs Session::specLinkedDocs() const {
    const std::string dir = regen::parentDir(selfPath());
    return [this, dir](const std::string& file) -> const doc::Document* {
        const auto it = links_.find(regen::linkPath(dir, file));
        if (it == links_.end() || !it->second.doc) return nullptr;
        return static_cast<const doc::Document*>(it->second.doc);
    };
}

regen::StatePtr Session::specAssemblyState(const doc::Element& spec) const {
    const PartResultPtr pr = regenResult();
    if (!pr || spec.specAssembly.empty()) return nullptr;
    const doc::Element& act = doc().active();
    if (act.id == spec.id) return pr->specAssembly;
    if (act.id == spec.specAssembly && act.isAssembly()) return pr->final;
    if (act.isDrawing())  // a view model of the assembly (regen.md §3а)
        if (const auto it = pr->models.find(regen::drawingModelKey("", spec.specAssembly)); it != pr->models.end() && !it->second.error)
            return it->second.state;
    return nullptr;
}

drawing::SpecContent Session::specContent(const doc::Element& spec) const {
    const regen::StatePtr st = specAssemblyState(spec);
    return drawing::specificationContent(doc(), spec, specLinkedDocs(), st ? &st->connections : nullptr);
}

Status Session::updateSpecification(const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    const doc::Element& el = doc().active();
    if (!el.isSpecification()) return notSpecification(el);
    const std::string id = el.id;
    if (!def.is_object()) return invalidDefinition("definition must be an object");
    for (auto it = def.begin(); it != def.end(); ++it) {
        const std::string& k = it.key();
        if (k != "designation" && k != "assembly" && k != "titleBlock" && k != "rows" && k != "extraRows")
            return invalidDefinition(k + " not allowed", "/" + k);
    }
    // Merging with the element (core-api §4.1е): null removes an optional field, otherwise replaces it.
    Json dj = doc::documentToJsonValue(doc());
    const int ei = doc().activeIndex();
    Json& ej = dj["elements"][size_t(ei)];
    mergePatch(ej, def);
    // Empty arrays and the title block object are not written (document.md §2.4е).
    for (const char* k : {"titleBlock", "rows", "extraRows"})
        if (ej.contains(k) && ej[k].empty()) ej.erase(k);
    auto r = doc::documentFromJson(dj);
    if (!r) return schemaErrorAt(r.error(), "/elements/" + std::to_string(ei));
    r->activeElement = doc().activeElement;  // documentFromJson makes the first element active
    return commit(std::move(*r), "undo.updateSpecification", {}, id);
}

Result<Session::SpecificationInfo> Session::specification(const std::string& element) const {
    const int i = element.empty() ? doc().activeIndex() : doc().elementIndex(element);
    if (i < 0) return notFound(element);
    const doc::Element& el = doc().elements[size_t(i)];
    if (!el.isSpecification()) return notSpecification(el);
    SpecificationInfo info;
    info.content = specContent(el);
    info.pages = int(drawing::specificationPages(info.content).size());
    return info;
}

Result<drawing::SheetGraphics> Session::specificationSheet(const std::string& pageId) const {
    const doc::Element& el = doc().active();
    const auto content = specContent(el);
    const auto pages = drawing::specificationPages(content);
    const int page = drawing::specPageIndex(pageId);
    if (page < 0 || page >= int(pages.size())) return notFound(pageId);
    return drawing::composeSpecificationPage(doc(), el, content, pages, page);
}

Result<std::map<std::string, std::string>> Session::specificationTitleDefaults(const std::string& pageId) const {
    const doc::Element& el = doc().active();
    const auto pages = drawing::specificationPages(specContent(el));
    const int page = drawing::specPageIndex(pageId);
    if (page < 0 || page >= int(pages.size())) return notFound(pageId);
    return drawing::specificationTitleDefaults(doc(), el, page, int(pages.size()));
}

}  // namespace construe::api
