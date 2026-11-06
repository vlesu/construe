// Session: copying objects between documents, «Make local copy», «Replace with…»
// (spec/core-api.md §4.1в, §4.1г; spec/assembly.md §5б; FX7-ASM-008…010).
#include "api/session.h"

#include "api/errors.h"
#include "base/files.h"
#include "doc/element_copy.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace construe::api {

namespace {

Result<doc::Document> readDocument(const std::string& abs) {
    std::ifstream f(std::filesystem::u8path(abs), std::ios::binary);
    if (!f) return Error("IO_READ").with("detail", std::string("cannot open")).with("file", abs);
    std::stringstream ss;
    ss << f.rdbuf();
    return doc::parse(ss.str());
}

// Base view of a drawing a view derives from (through parent); nullptr — none.
const doc::DrawingView* baseOf(const doc::Element& drawing, const std::string& viewId, int depth = 0) {
    if (depth > 64) return nullptr;
    for (const auto& sh : drawing.sheets)
        for (const auto& v : sh.views)
            if (v.id == viewId) return v.kind == "base" ? &v : baseOf(drawing, v.parent, depth + 1);
    return nullptr;
}

doc::DrawingView* findView(doc::Element& drawing, const std::string& id) {
    for (auto& sh : drawing.sheets)
        for (auto& v : sh.views)
            if (v.id == id) return &v;
    return nullptr;
}

bool insertable(const std::string& kind) { return kind == "part" || kind == "body" || kind == "assembly"; }

}  // namespace

Result<Json> Session::copyElement(const std::string& id) const {
    const int i = doc().elementIndex(id);
    if (i < 0) return notFound(id);
    const doc::Element& e = doc().elements[size_t(i)];
    if (e.isSpecification()) return badArg("element");
    return Json{{"format", "construe-element"},
                {"version", 1},
                {"source", selfPath()},
                {"element", id},
                {"kind", e.kind},
                {"name", e.name},
                {"dependencies", doc::allDependencies(doc(), id).size()},
                {"document", doc::documentToJsonValue(doc())}};
}

Result<std::string> Session::pasteElement(const Json& data, const std::string& mode) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    if (mode != "copy" && mode != "link") return badArg("mode");
    if (!data.is_object() || data.value("format", std::string()) != "construe-element" || data.value("version", 0) != 1 ||
        !data.contains("element") || !data["element"].is_string() || !data.contains("document") || !data["document"].is_object() ||
        !data.contains("source") || !data["source"].is_string())
        return badArg("data");
    auto src = doc::documentFromJson(data["document"]);
    if (!src) return badArg("data");
    const std::string source = data["source"].get<std::string>().empty() ? std::string() : absoluteU8(data["source"]);
    const std::string self = selfPath();
    const std::string dir = regen::parentDir(self);
    doc::CopyOptions o;
    o.roots = {data["element"].get<std::string>()};
    o.copyDependencies = mode == "copy";
    o.sameDocument = !self.empty() && source == self;
    if (!self.empty() && !source.empty()) o.sourceLink = relativeU8(source, dir);
    o.rebase = [&](const std::string& file) -> std::optional<std::string> {
        if (self.empty()) return std::nullopt;
        const std::string abs = regen::linkPath(regen::parentDir(source), file);
        if (abs == self) return std::string();
        return relativeU8(abs, dir);
    };
    auto r = doc::copyElements(doc(), *src, o);
    if (!r) return r.error();
    doc::Document nd = std::move(r->document);
    const std::string id = r->roots.at(0);
    nd.activeElement = id;
    if (auto s = commit(std::move(nd), "undo.pasteElement", {}, id); !s) return s.error();
    return id;
}

Result<std::string> Session::localizeInstance(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    const doc::Instance* in = nullptr;
    for (const auto& x : doc().active().instances)
        if (x.id == id) in = &x;
    if (!in) return notFound(id);
    if (in->refFile.empty() || doc::isLibraryRef(in->refFile)) return badArg("ref");
    const std::string self = selfPath();
    const std::string dir = regen::parentDir(self);
    const std::string abs = regen::linkPath(dir, in->refFile);
    const std::string asmId = doc().active().id;
    const std::string sourceRoot = in->refElement;
    doc::Document nd;
    std::string local;
    std::optional<doc::CopyResult> copy;
    std::optional<doc::Document> src;
    if (abs == self) {  // a reference to this very file: just a local reference
        nd = doc();
        local = sourceRoot;
    } else {
        auto rd = readDocument(abs);
        if (!rd) return rd.error();
        src = std::move(*rd);
        doc::CopyOptions o;
        o.roots = {sourceRoot};
        o.copyDependencies = true;
        o.dedupeRoots = true;
        o.sourceLink = relativeU8(abs, dir);
        o.rebase = [&](const std::string& file) -> std::optional<std::string> {
            const std::string a2 = regen::linkPath(regen::parentDir(abs), file);
            if (a2 == self) return std::string();
            return relativeU8(a2, dir);
        };
        for (const auto& ov : in->overrides) o.usedIds.push_back(ov.variable);
        for (const auto& vr : in->variantRows) o.usedIds.push_back(vr.table);
        auto r = doc::copyElements(doc(), *src, o);
        if (!r) return r.error();
        copy = std::move(*r);
        nd = std::move(copy->document);
        local = copy->roots.at(0);
    }
    doc::Element& a = nd.elements[size_t(nd.elementIndex(asmId))];
    for (auto& x : a.instances) {
        if (x.id != id) continue;
        x.refFile.clear();
        x.refElement = local;
        if (copy) {  // the copied document's variables and tables are now this document's (§5б.3)
            for (auto& ov : x.overrides)
                if (auto it = copy->ids.find(ov.variable); it != copy->ids.end()) ov.variable = it->second;
            for (auto& vr : x.variantRows)
                if (auto it = copy->ids.find(vr.table); it != copy->ids.end()) vr.table = it->second;
        }
    }
    if (copy) {
        // References "<id>/<name>" to the copied object's geometry: the names of the copy (§5б.3).
        const auto fn = [&](const std::string& rest) { return doc::rewriteReference(rest, *src, sourceRoot, *copy); };
        for (auto& el : nd.elements) {
            const bool owner = el.id == asmId;
            const bool context = el.context && el.context->assembly == asmId;
            bool drawing = false;
            for (const auto& sh : el.sheets)
                for (const auto& v : sh.views)
                    if (v.kind == "base" && v.refFile.empty() && v.refElement == asmId) drawing = true;
            if (!owner && !context && !drawing) continue;
            if (drawing && !owner && !context) {
                for (auto& sh : el.sheets)
                    for (auto& dim : sh.dimensions) {
                        const doc::DrawingView* b = baseOf(el, dim.view);
                        if (!b || !b->refFile.empty() || b->refElement != asmId) continue;
                        Json j = doc::dimensionToJsonValue(dim);
                        doc::rewritePrefixed(j["refs"], id, fn);
                        dim = doc::dimensionFromJsonValue(j);
                    }
                continue;
            }
            Json j = doc::elementToJsonValue(el);
            doc::rewritePrefixed(j, id, fn);
            el = doc::elementFromJsonValue(j);
        }
        if (auto s = doc::validate(nd); !s) return s.error();
    }
    if (auto s = commit(std::move(nd), "undo.localize", {}, id); !s) return s.error();
    return local;
}

Result<std::string> Session::localizeView(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    doc::Document nd = doc();
    doc::DrawingView* v = findView(nd.active(), id);
    if (!v) return notFound(id);
    if (v->kind != "base" || v->refFile.empty()) return badArg("ref");
    const std::string self = selfPath();
    const std::string dir = regen::parentDir(self);
    const std::string abs = regen::linkPath(dir, v->refFile);
    const std::string drawingId = nd.active().id;
    const std::string sourceRoot = v->refElement;
    if (abs == self) {
        v->refFile.clear();
        if (auto s = commit(std::move(nd), "undo.localize", {}, id); !s) return s.error();
        return sourceRoot;
    }
    auto src = readDocument(abs);
    if (!src) return src.error();
    doc::CopyOptions o;
    o.roots = {sourceRoot};
    o.copyDependencies = true;
    o.dedupeRoots = true;
    o.sourceLink = relativeU8(abs, dir);
    o.rebase = [&](const std::string& file) -> std::optional<std::string> {
        const std::string a2 = regen::linkPath(regen::parentDir(abs), file);
        if (a2 == self) return std::string();
        return relativeU8(a2, dir);
    };
    auto r = doc::copyElements(doc(), *src, o);
    if (!r) return r.error();
    nd = std::move(r->document);
    const std::string local = r->roots.at(0);
    doc::Element& drawing = nd.elements[size_t(nd.elementIndex(drawingId))];
    findView(drawing, id)->refFile.clear();
    findView(drawing, id)->refElement = local;
    // Dimensions of the view and of the views derived from it: names of the copy (§5б.3).
    for (auto& sh : drawing.sheets)
        for (auto& dim : sh.dimensions) {
            const doc::DrawingView* b = baseOf(drawing, dim.view);
            if (!b || b->id != id) continue;
            for (auto& ref : dim.refs) ref.name = doc::rewriteReference(ref.name, *src, sourceRoot, *r);
        }
    if (auto s = doc::validate(nd); !s) return s.error();
    if (auto s = commit(std::move(nd), "undo.localize", {}, id); !s) return s.error();
    return local;
}

Status Session::replaceInstance(const std::string& id, const Json& ref) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    bool found = false;
    for (const auto& x : doc().active().instances) found = found || x.id == id;
    if (!found) return notFound(id);
    auto rr = resolveRef(ref, true);
    if (!rr) return rr.error();
    if (!insertable(rr->kind)) return badArg("ref");
    doc::Document nd = doc();
    for (auto& x : nd.active().instances) {
        if (x.id != id) continue;
        const bool otherDocument = x.refFile != rr->file;
        x.refFile = rr->file;
        x.refElement = rr->element;
        if (otherDocument) {  // instance variables refer to the previous document (assembly.md §5б.4)
            x.overrides.clear();
            x.variantRows.clear();
        }
    }
    return commit(std::move(nd), "undo.replaceRef", {}, id);
}

Status Session::replaceView(const std::string& id, const Json& ref) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    doc::Document nd = doc();
    doc::DrawingView* v = findView(nd.active(), id);
    if (!v) return notFound(id);
    if (v->kind != "base") return badArg("id");
    auto rr = resolveRef(ref, false);
    if (!rr) return rr.error();
    if (!insertable(rr->kind)) return badArg("ref");
    v->refFile = rr->file;
    v->refElement = rr->element;
    return commit(std::move(nd), "undo.replaceRef", {}, id);
}

}  // namespace construe::api
