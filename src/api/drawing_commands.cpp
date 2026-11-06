// Session: drawings - sheets, views, sheet primitives (spec/core-api.md §4.1г, spec/drawing.md).
#include "api/errors.h"
#include "api/session.h"

#include "base/strings.h"
#include "base/thread.h"
#include "doc/json_text.h"
#include "drawing/cut_plan.h"
#include "drawing/dimensions.h"
#include "drawing/sheet_marks.h"
#include "drawing/sheet_parts.h"
#include "drawing/standard.h"
#include "drawing/title_block.h"
#include "regen/regen.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <thread>

namespace construe::api {

namespace {

Status checkNoIdName(const Json& def) {
    if (!def.is_object()) return Error("API_INVALID_DEFINITION").with("detail", std::string("definition must be an object"));
    for (const char* k : {"id", "name"})
        if (def.contains(k)) return Error("API_INVALID_DEFINITION").with("detail", std::string(k) + " not allowed").at(std::string("/") + k);
    return okStatus();
}

struct Located {
    int sheet = -1, view = -1;
};

// Constructions matching pred and the dimensions on them (core-api §4.1г, drawing.md §6.29, FX14-DRW-006).
template <class Pred>
void eraseConstructions(doc::Sheet& sh, Pred pred) {
    std::set<std::string> gone;
    for (const auto& c : sh.constructions)
        if (pred(c)) gone.insert(c.id);
    if (gone.empty()) return;
    std::erase_if(sh.constructions, [&](const doc::SheetConstruction& c) { return gone.count(c.id) > 0; });
    std::erase_if(sh.dimensions, [&](const doc::SheetDimension& a) {
        return std::any_of(a.refs.begin(), a.refs.end(), [&](const doc::DimensionRef& r) { return r.kind == "construction" && gone.count(r.name) > 0; });
    });
}
Located locate(const doc::Element& el, const std::string& id) {
    for (size_t si = 0; si < el.sheets.size(); ++si) {
        if (el.sheets[si].id == id) return {int(si), -1};
        const int vi = el.sheets[si].viewIndex(id);
        if (vi >= 0) return {int(si), vi};
    }
    return {};
}

// Document with the JSON of sheet si of the active element replaced - via the schema (core-api.md §4.1г).
Result<doc::Document> withSheetJson(const doc::Document& base, size_t si, const Json& sheetJson) {
    Json dj = doc::documentToJsonValue(base);
    const int ei = base.activeIndex();
    Json& ss = dj["elements"][ei]["sheets"];
    if (si < ss.size()) ss[si] = sheetJson;
    else ss.push_back(sheetJson);
    auto r = doc::documentFromJson(dj);
    if (!r) return schemaErrorAt(r.error(), "/elements/" + std::to_string(ei) + "/sheets/" + std::to_string(si));
    r->activeElement = base.activeElement;  // documentFromJson makes the first element active
    return r;
}

// Session model source (core-api.md §5.4): models - from the drawing's regeneration result, projections - via cache.
// compute = false (owner thread): cache miss - "not computed yet"; true (worker thread) - compute and put in cache.
// With misses set (worker pre-pass, FX8-DRW-001), a miss is recorded instead of computed - the worker then computes
// the recorded requests in parallel.
class SessionModels final : public drawing::ModelProvider {
public:
    using Models = std::map<std::string, regen::DrawingModel>;
    struct Request {
        bool section = false;
        std::vector<kernel::PlacedShape> bodies;
        kernel::ViewDirection view;
        Point3 origin = Point3::Zero();
        kernel::HlrParams params;
        std::optional<ProjectionCache::Key> key;
    };
    SessionModels(const kernel::Kernel& k, std::shared_ptr<const Models> models, std::string self, ProjectionCache& cache, bool compute,
                  const Cancel* cancel, const regen::LinkMap* links = nullptr, std::vector<Request>* misses = nullptr)
        : k_(k), models_(std::move(models)), self_(std::move(self)), cache_(cache), compute_(compute), cancel_(cancel), links_(links),
          misses_(misses) {}

    Result<regen::StatePtr> model(const std::string& file, const std::string& element) const override {
        auto m = find(file, element);
        if (!m) return m.error();
        return (*m)->state;
    }
    Result<std::vector<kernel::PlacedShape>> modelBodies(const std::string& file, const std::string& element) const override {
        auto m = find(file, element);
        if (!m) return m.error();
        return (*m)->bodies;
    }
    std::vector<std::string> modelColors(const std::string& file, const std::string& element) const override {
        auto m = find(file, element);
        if (!m) return {};
        return (*m)->colors;
    }
    std::vector<regen::DrawingCutFace> modelCutFaces(const std::string& file, const std::string& element) const override {
        auto m = find(file, element);
        if (!m) return {};
        return (*m)->cutFaces;
    }
    Result<std::shared_ptr<const kernel::HlrProjection>> projection(const std::vector<kernel::PlacedShape>& bodies,
                                                                   const kernel::ViewDirection& view,
                                                                   const kernel::HlrParams& params) const override {
        auto key = cache_.makeKey(k_, bodies, view, params.chordTol);
        if (key)
            if (auto hit = cache_.get(*key)) return hit;
        if (misses_) {
            misses_->push_back({false, bodies, view, Point3::Zero(), params, key});
            return Error(drawing::VIEW_PENDING);
        }
        if (!compute_ || cancelled(cancel_)) return Error(drawing::VIEW_PENDING);
        return computeProjection(k_, cache_, bodies, view, params, key, cancel_);
    }
    Result<std::shared_ptr<const kernel::SectionCut>> sectionCut(const std::vector<kernel::PlacedShape>& bodies,
                                                                const kernel::ViewDirection& view, const Point3& origin,
                                                                const kernel::HlrParams& params) const override {
        auto key = cache_.makeKey(k_, bodies, view, params.chordTol);
        if (key)
            if (auto hit = cache_.getSection({*key, origin})) return hit;
        if (misses_) {
            misses_->push_back({true, bodies, view, origin, params, key});
            return Error(drawing::VIEW_PENDING);
        }
        if (!compute_ || cancelled(cancel_)) return Error(drawing::VIEW_PENDING);
        return computeSection(k_, cache_, bodies, view, origin, params, key, cancel_);
    }
    // Kernel computation of a request and its cache entry (no key - not cached).
    static Result<std::shared_ptr<const kernel::HlrProjection>> computeProjection(const kernel::Kernel& k, ProjectionCache& cache,
                                                                                const std::vector<kernel::PlacedShape>& bodies,
                                                                                const kernel::ViewDirection& view,
                                                                                const kernel::HlrParams& params,
                                                                                const std::optional<ProjectionCache::Key>& key,
                                                                                const Cancel* cancel) {
        cache.countProjection();
        auto r = k.hiddenLineProjection(bodies, view, params, cancel);
        if (!r) {
            if (r.error().code == "KERNEL_CANCELLED") return Error(drawing::VIEW_PENDING);
            return r.error();
        }
        auto v = std::make_shared<const kernel::HlrProjection>(std::move(*r));
        if (key) cache.put(*key, v);
        return v;
    }
    static Result<std::shared_ptr<const kernel::SectionCut>> computeSection(const kernel::Kernel& k, ProjectionCache& cache,
                                                                           const std::vector<kernel::PlacedShape>& bodies,
                                                                           const kernel::ViewDirection& view, const Point3& origin,
                                                                           const kernel::HlrParams& params,
                                                                           const std::optional<ProjectionCache::Key>& key,
                                                                           const Cancel* cancel) {
        cache.countSection();
        auto r = k.sectionCut(bodies, view, origin, params, cancel);
        if (!r) {
            if (r.error().code == "KERNEL_CANCELLED") return Error(drawing::VIEW_PENDING);
            return r.error();
        }
        auto v = std::make_shared<const kernel::SectionCut>(std::move(*r));
        if (key) cache.putSection({*key, origin}, v);
        return v;
    }
    const kernel::Kernel& kernel() const override { return k_; }
    // Document of a linked file (dimensions from the model of other files, drawing.md §6а.10 item 1).
    const doc::Document* linkedDocument(const std::string& file) const override {
        if (!links_ || file.empty()) return nullptr;
        const auto it = links_->find(regen::linkPath(regen::parentDir(self_), file));
        if (it == links_->end() || it->second.error) return nullptr;
        return static_cast<const doc::Document*>(it->second.doc);
    }

private:
    Result<const regen::DrawingModel*> find(const std::string& file, const std::string& element) const {
        if (!models_) return Error(drawing::VIEW_PENDING);
        const std::string abs = file.empty() ? std::string() : regen::linkPath(regen::parentDir(self_), file);
        const auto it = models_->find(regen::drawingModelKey(abs, element));
        if (it == models_->end()) return Error(drawing::VIEW_PENDING);
        if (it->second.error) return *it->second.error;
        return &it->second;
    }

    const kernel::Kernel& k_;
    std::shared_ptr<const Models> models_;
    std::string self_;
    ProjectionCache& cache_;
    bool compute_;
    const Cancel* cancel_;
    const regen::LinkMap* links_;
    std::vector<Request>* misses_;
};

bool precomputeSheets(const kernel::Kernel& k, ProjectionCache& cache, const doc::Document& d,
                      const std::shared_ptr<const SessionModels::Models>& ms, const std::string& self, const Cancel* c,
                      const regen::LinkMap* links);

}  // namespace

// ---------------------------------------------------------------- §4.1г sheets

Result<std::string> Session::addSheet(const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    if (auto s = checkNoIdName(def); !s) return s.error();
    doc::Document nd = doc();
    const doc::Sheet& last = nd.active().sheets.back();
    Json sj = Json::object();
    sj["id"] = doc::allocate(nd, doc::IdKind::Sheet);
    sj["name"] = doc::defaultSheetName(nd.active());
    sj["format"] = last.format;
    sj["orientation"] = last.orientation;
    sj["scale"] = last.scale;
    if (last.scaleManual || def.contains("scale")) sj["scaleManual"] = true;  // FX10-DRW-001
    mergePatch(sj, def);
    const std::string id = sj["id"];
    auto r = withSheetJson(nd, nd.active().sheets.size(), sj);
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.addSheet", {}, id); !s) return s.error();
    return id;
}

Status Session::updateSheet(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(id);
    if (si < 0) return notFound(id);
    if (auto s = checkNoIdName(def); !s) return s;
    if (def.contains("views")) return Error("API_INVALID_DEFINITION").with("detail", std::string("views not allowed")).at("/views");
    Json sj = doc::sheetToJsonValue(doc().active().sheets[size_t(si)]);
    mergePatch(sj, def);
    if (def.contains("scale")) sj["scaleManual"] = true;  // the user's scale: three views keep it (FX10-DRW-001)
    auto r = withSheetJson(doc(), size_t(si), sj);
    if (!r) return r.error();
    return commit(std::move(*r), "undo.updateSheet", {}, id);
}

Status Session::renameSheet(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(id);
    if (si < 0) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    doc::Document nd = doc();
    nd.active().sheets[size_t(si)].name = n;
    return commit(std::move(nd), "undo.renameSheet", {}, id);
}

Status Session::deleteSheet(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(id);
    if (si < 0) return notFound(id);
    if (doc().active().sheets.size() == 1) return Error("API_LAST_SHEET").with("id", id);
    doc::Document nd = doc();
    nd.active().sheets.erase(nd.active().sheets.begin() + si);
    return commit(std::move(nd), "undo.deleteSheet", {}, id);
}

// ---------------------------------------------------------------- §4.1г views

namespace {
// Orientation "plane" (FX7-DRW-007, drawing.md §6.1): toward the viewer — the plane normal, up — +Y projected onto the
// plane (along +Y — −sign(n.y)·Z), as the standard orientations.
Result<std::pair<Vec3, Vec3>> planeAxes(const drawing::ModelProvider& models, const std::string& file, const std::string& element,
                                        const Json& plane) {
    auto bad = [] { return Error("API_INVALID_DEFINITION").with("detail", std::string("plane")); };
    if (!plane.is_object()) return bad();
    std::optional<Plane> pl;
    if (plane.contains("datum")) {
        pl = sketch::datumPlane(plane["datum"].get<std::string>());
    } else {
        auto st = models.model(file, element);
        if (!st) return st.error();
        if (!*st) return bad();
        if (plane.contains("feature")) {
            const auto it = (*st)->planes.find(plane["feature"].get<std::string>());
            if (it != (*st)->planes.end()) pl = it->second;
        } else if (plane.contains("face")) {
            std::vector<regen::Message> warnings;
            auto r = regen::sketchPlaneOnFace(plane["face"].get<std::string>(), **st, models.kernel(), warnings);
            if (!r) return r.error();
            pl = *r;
        }
    }
    if (!pl || !(pl->normal.norm() > 0)) return bad();
    const Vec3 n = pl->normal.normalized();
    Vec3 u = Vec3::UnitY() - Vec3::UnitY().dot(n) * n;
    if (u.norm() < 1e-9) u = (n.y() > 0 ? -1.0 : 1.0) * Vec3::UnitZ();
    return std::pair{n, u.normalized()};
}
}  // namespace

// The merged definition of a main view (FX7-DRW-007): orientation other than "plane" drops plane, normal, up; a new
// plane (in def) gives orientation "plane" and resolves normal and up from the view model.
Status Session::prepareViewPlane(Json& vj, const Json& def) const {
    if (vj.value("kind", std::string()) != "base") return okStatus();
    if (def.contains("plane") && !def["plane"].is_null()) {
        std::shared_ptr<const SessionModels::Models> ms;
        {
            std::lock_guard lk(mu_);
            ms = drawingModels_;
        }
        SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr);
        const Json ref = vj.value("ref", Json::object());
        auto axes = planeAxes(models, ref.value("file", std::string()), ref.value("element", std::string()), def["plane"]);
        if (!axes) return axes.error();
        vj["orientation"] = "plane";
        vj["normal"] = Json::array({axes->first.x(), axes->first.y(), axes->first.z()});
        vj["up"] = Json::array({axes->second.x(), axes->second.y(), axes->second.z()});
    } else if (vj.value("orientation", std::string()) != "plane") {
        vj.erase("plane");
        vj.erase("normal");
        vj.erase("up");
    }
    return okStatus();
}

Result<std::string> Session::addView(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    if (auto s = checkNoIdName(def); !s) return s.error();
    // Cut plan (drawing.md §2.7 «Добавление детали», FX11-CUT-001, FX11-CUT-004): only sheet metal parts of the sheet's
    // thickness; without a position — in a row.
    std::optional<Point2> cutPos;
    if (drawing::isCutPlan(doc().active())) {
        if (def.value("kind", std::string()) != "base") return Error("DRAWING_CUT_VIEW_KIND").with("id", std::string());
        const Json ref = def.value("ref", Json::object());
        if (!ref.is_object() || !ref.contains("element") || !ref["element"].is_string())
            return invalidDefinition("ref.element required", "/ref");
        const std::string file = ref.value("file", std::string()), element = ref["element"].get<std::string>();
        // Model state: of the published drawing regeneration, else a regeneration of the model now.
        auto stateOf = [&](const std::string& f, const std::string& e) -> regen::StatePtr {
            const std::string abs = f.empty() ? std::string() : regen::linkPath(regen::parentDir(selfPath()), f);
            {
                std::lock_guard lk(mu_);
                if (drawingModels_)
                    if (const auto it = drawingModels_->find(regen::drawingModelKey(abs, e)); it != drawingModels_->end())
                        return it->second.state;
            }
            const doc::Document* D = &doc();
            std::string path = selfPath();
            if (!f.empty()) {
                const auto it = links_.find(abs);
                if (it == links_.end() || it->second.error || !it->second.doc) return nullptr;
                D = static_cast<const doc::Document*>(it->second.doc);
                path = abs;
            }
            const int ei = D->elementIndex(e);
            if (ei < 0 || D->elements[size_t(ei)].kind != "part") return nullptr;
            doc::Document pd = *D;
            pd.activeElement = e;
            regen::Regenerator rg(k_);
            auto r = rg.regenerate(pd, revision_, nullptr, false, &links_, path);
            return r ? r->final : nullptr;
        };
        auto part = drawing::cutPart(k_, stateOf(file, element), nullptr, element, file);
        if (!part) return part.error();
        const doc::Sheet& sh = doc().active().sheets[size_t(si)];
        for (const auto& v : sh.views) {
            if (v.kind != "base") continue;
            auto other = drawing::cutPart(k_, stateOf(v.refFile, v.refElement), nullptr, v.refElement, v.refFile);
            if (!other) continue;
            if (std::abs(other->thickness - part->thickness) > drawing::CUT_THICKNESS_TOL)
                return Error("DRAWING_CUT_THICKNESS").with("element", element).with("thickness", part->thickness).with("sheet", other->thickness);
            break;
        }
        if (!def.contains("position")) {
            auto g = sheetGraphics(sheetId);
            const Point2 size = part->max - part->min;
            cutPos = drawing::cutRowPosition(sh, g ? *g : drawing::SheetGraphics{}, size.x(), size.y());
        }
    }
    doc::Document nd = doc();
    Json vj = Json::object();
    vj["id"] = doc::allocate(nd, doc::IdKind::View);
    vj["name"] = doc::defaultViewName(nd.active());
    mergePatch(vj, def);
    if (cutPos) vj["position"] = Json::array({cutPos->x(), cutPos->y()});
    if (drawing::isCutPlan(doc().active()) && !vj.contains("orientation")) vj["orientation"] = "top";  // does not apply (§2.7)
    if (auto s = prepareViewPlane(vj, def); !s) return s.error();
    const std::string id = vj["id"];
    Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
    if (!sj.contains("views")) sj["views"] = Json::array();
    sj["views"].push_back(vj);
    const size_t vi = sj["views"].size() - 1;
    auto r = withSheetJson(nd, size_t(si), sj);
    if (!r) {
        Error e = r.error();
        if (e.path) {  // path inside the sheet → inside the view definition
            const std::string vp = "/views/" + std::to_string(vi);
            e.path = e.path->rfind(vp, 0) == 0 ? std::optional<std::string>(e.path->substr(vp.size())) : std::nullopt;
            if (e.path && e.path->empty()) e.path.reset();
        }
        return e;
    }
    if (const std::string kind = vj.value("kind", std::string()); kind == "projected" || kind == "section" || kind == "detail") {
        const auto& views = nd.active().sheets[size_t(si)].views;
        const std::string parent = vj.value("parent", std::string());
        if (std::none_of(views.begin(), views.end(), [&](const doc::DrawingView& v) { return v.id == parent; }))
            return notFound(parent);
    } else if (kind == "base") {
        const Json ref = vj.value("ref", Json::object());
        autoNameDrawing(*r, ref.value("file", std::string()), ref.value("element", std::string()));  // FX9-DRW-003
    }
    if (auto s = commit(std::move(*r), "undo.addView", {}, id); !s) return s.error();
    return id;
}

Status Session::updateView(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locate(doc().active(), id);
    if (at.view < 0) return notFound(id);
    if (auto s = checkNoIdName(def); !s) return s;
    const doc::Sheet& oldSheet = doc().active().sheets[size_t(at.sheet)];
    Json sj = doc::sheetToJsonValue(oldSheet);
    // FX10-DRW-003 (drawing.md §6.26): projected views that inherit the section list of this view keep what they show —
    // they get its list before the change (none in the chain — the sections enabled in the model, when it is known).
    if (def.contains("sections")) {
        std::optional<std::vector<std::string>> before = doc::viewSectionsList(oldSheet, id);
        if (!before)
            if (const doc::DrawingView* bv = doc::chainBaseView(oldSheet, id)) {
                const doc::Element* model = nullptr;
                if (bv->refFile.empty()) {
                    if (const int ei = doc().elementIndex(bv->refElement); ei >= 0) model = &doc().elements[size_t(ei)];
                } else if (const auto it = links_.find(regen::linkPath(regen::parentDir(selfPath()), bv->refFile));
                           it != links_.end() && !it->second.error && it->second.doc) {
                    const auto* ld = static_cast<const doc::Document*>(it->second.doc);
                    if (const int ei = ld->elementIndex(bv->refElement); ei >= 0) model = &ld->elements[size_t(ei)];
                }
                if (model) before = doc::effectiveViewSections(*model, std::nullopt);
            }
        if (before)
            for (size_t i = 0; i < oldSheet.views.size(); ++i) {
                const auto& w = oldSheet.views[i];
                if (w.kind == "projected" && w.parent == id && !w.sections) sj["views"][i]["sections"] = *before;
            }
    }
    // A moved view carries its item numbers (FX14-DRW-002, drawing.md §6.27): their shelves shift by the same vector.
    if (def.is_object() && def.contains("position") && def["position"].is_array() && def["position"].size() == 2 &&
        def["position"][0].is_number() && def["position"][1].is_number() && sj.contains("marks")) {
        const auto& old = oldSheet.views[size_t(at.view)].position;
        const double dx = def["position"][0].get<double>() - old[0], dy = def["position"][1].get<double>() - old[1];
        for (auto& m : sj["marks"])
            if (m.value("kind", std::string()) == "balloon" && m.value("view", std::string()) == id && m.contains("position"))
                m["position"] = Json::array({m["position"][0].get<double>() + dx, m["position"][1].get<double>() + dy});
    }
    mergePatch(sj["views"][size_t(at.view)], def);
    if (auto s = prepareViewPlane(sj["views"][size_t(at.view)], def); !s) return s;
    auto r = withSheetJson(doc(), size_t(at.sheet), sj);
    if (!r) {
        Error e = r.error();
        if (e.path) {
            const std::string vp = "/views/" + std::to_string(at.view);
            e.path = e.path->rfind(vp, 0) == 0 ? std::optional<std::string>(e.path->substr(vp.size())) : std::nullopt;
            if (e.path && e.path->empty()) e.path.reset();
        }
        return e;
    }
    return commit(std::move(*r), "undo.updateView", {}, id);
}

Session::ViewSections Session::viewSections(const std::string& viewId) const {
    ViewSections out;
    const doc::Element& el = doc().active();
    const Located at = locate(el, viewId);
    if (at.view < 0) return out;
    const doc::Sheet& sh = el.sheets[size_t(at.sheet)];
    const doc::DrawingView* bv = doc::chainBaseView(sh, viewId);
    if (!bv) return out;
    out.found = true;
    out.list = doc::viewSectionsList(sh, viewId);
    out.baseElement = bv->refElement;
    out.baseFile = bv->refFile;
    out.baseFlat = bv->flat;
    return out;
}

Status Session::renameView(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locate(doc().active(), id);
    if (at.view < 0) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    doc::Document nd = doc();
    nd.active().sheets[size_t(at.sheet)].views[size_t(at.view)].name = n;
    return commit(std::move(nd), "undo.renameView", {}, id);
}

Status Session::deleteView(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locate(doc().active(), id);
    if (at.view < 0) return notFound(id);
    doc::Document nd = doc();
    auto& views = nd.active().sheets[size_t(at.sheet)].views;
    // The view and all views depending on it via parent (directly or through others).
    std::set<std::string> gone{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& v : views)
            if (v.kind != "base" && gone.count(v.parent) && gone.insert(v.id).second) grew = true;
    }
    std::erase_if(views, [&](const doc::DrawingView& v) { return gone.count(v.id) > 0; });
    // Dimensions of deleted views (§4.1г, S3-DRW-006).
    std::erase_if(nd.active().sheets[size_t(at.sheet)].dimensions, [&](const doc::SheetDimension& a) { return gone.count(a.view) > 0; });
    // Marks of deleted views (FX9-DRW-008), not dismissed: a new view of the model gets them again.
    std::erase_if(nd.active().sheets[size_t(at.sheet)].marks, [&](const doc::SheetMark& m) { return gone.count(m.view) > 0; });
    // Constructions of deleted views (FX14-DRW-006) and the dimensions on them.
    eraseConstructions(nd.active().sheets[size_t(at.sheet)], [&](const doc::SheetConstruction& c) { return gone.count(c.view) > 0; });
    return commit(std::move(nd), "undo.deleteView", {}, id);
}

// ---------------------------------------------------------------- §4.1г three projections (FX1-DRW-001)

Result<BBox3> Session::modelBox(const std::string& file, const std::string& element) {
    // Model - from the drawing's last regeneration; if none - regenerate a separate document with one main model view
    // on the owner thread (core-api §4.1г addThreeViews).
    const std::string abs = file.empty() ? std::string() : regen::linkPath(regen::parentDir(selfPath()), file);
    const std::string key = regen::drawingModelKey(abs, element);
    std::shared_ptr<const std::map<std::string, regen::DrawingModel>> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    std::vector<kernel::PlacedShape> bodies;
    const regen::DrawingModel* m = nullptr;
    std::optional<regen::PartResult> tmp;
    if (ms)
        if (auto it = ms->find(key); it != ms->end()) m = &it->second;
    if (!m) {
        doc::Document d = doc();
        doc::Element& el = d.active();
        doc::Sheet sh = el.sheets.front();
        sh.views.clear();
        sh.dimensions.clear();
        doc::DrawingView v;
        v.id = "dv_tmp";
        v.name = "tmp";
        v.kind = "base";
        v.refElement = element;
        v.refFile = file;
        v.orientation = "front";
        sh.views.push_back(v);
        el.sheets = {sh};
        loadLinks(&d);  // references of the temporary document: the new view's file (FX3-DRW-005)
        regen::Regenerator rg(k_);
        tmp = rg.regenerate(d, revision_, nullptr, false, &links_, selfPath());
        if (!tmp) return Error("API_STATE_UNAVAILABLE");
        if (auto it = tmp->models.find(key); it != tmp->models.end()) m = &it->second;
    }
    if (!m) return Error("API_STATE_UNAVAILABLE");
    if (m->error) return *m->error;
    BBox3 box;
    box.setEmpty();
    for (const auto& b : m->bodies) {
        const BBox3 bb = k_.bbox(b.shape);
        if (bb.isEmpty()) continue;
        Eigen::Matrix3d R;
        const auto& r = b.motion.r;
        R << r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8];
        for (int c = 0; c < 8; ++c) box.extend(Point3(R * bb.corner(static_cast<BBox3::CornerType>(c)) + b.motion.t));
    }
    return box;
}

Result<std::vector<std::string>> Session::addThreeViews(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    if (drawing::isCutPlan(doc().active())) return Error("DRAWING_CUT_VIEW_KIND").with("id", std::string());  // drawing.md §2.7
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    if (!def.is_object() || !def.contains("ref") || !def["ref"].is_object() || !def["ref"].contains("element") || !def["ref"]["element"].is_string())
        return Error("API_INVALID_DEFINITION").with("detail", std::string("ref")).at("/ref");
    if (!def.contains("position") || !def["position"].is_array() || def["position"].size() != 2 || !def["position"][0].is_number() ||
        !def["position"][1].is_number())
        return Error("API_INVALID_DEFINITION").with("detail", std::string("position")).at("/position");
    const std::string element = def["ref"]["element"];
    const std::string file = def["ref"].value("file", std::string());
    auto box = modelBox(file, element);
    if (!box) return box.error();
    const doc::Element& el = doc().active();
    const doc::Sheet& sheet = el.sheets[size_t(si)];
    const Point2 want(def["position"][0].get<double>(), def["position"][1].get<double>());
    // Fitting into the sheet (drawing.md §6.2б, FX10-DRW-001): the nearest free position; the scale of a sheet without
    // views that the user has not set goes down the standard series until the views fit.
    const bool autoScale = !sheet.scaleManual && sheet.views.empty();
    const auto fit = drawing::fitThreeViews(*box, sheet.scale, autoScale, drawing::projectionMethod(el), want, drawing::sheetField(el, sheet));
    const Point2 pos = fit.pos;
    const double k = doc::scaleValue(fit.scale).value_or(1.0);
    const auto lay = drawing::threeViewsLayout(*box, k, drawing::projectionMethod(el), pos);
    doc::Document nd = doc();
    Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
    if (fit.scale != sheet.scale) sj["scale"] = fit.scale;
    if (!sj.contains("views")) sj["views"] = Json::array();
    std::vector<std::string> ids;
    doc::Element names = nd.active();  // names "View N" - as in addView, in turn
    auto add = [&](Json vj) {
        vj["id"] = doc::allocate(nd, doc::IdKind::View);
        doc::DrawingView placeholder;
        placeholder.name = doc::defaultViewName(names);
        names.sheets.front().views.push_back(placeholder);
        vj["name"] = placeholder.name;
        ids.push_back(vj["id"]);
        sj["views"].push_back(vj);
    };
    Json ref = Json{{"element", element}};
    if (!file.empty()) ref["file"] = file;
    add(Json{{"kind", "base"}, {"ref", ref}, {"orientation", "front"}, {"position", {pos.x(), pos.y()}}});
    add(Json{{"kind", "projected"}, {"parent", ids[0]}, {"direction", lay.topDirection}, {"position", {lay.top.x(), lay.top.y()}}});
    add(Json{{"kind", "projected"}, {"parent", ids[0]}, {"direction", lay.sideDirection}, {"position", {lay.side.x(), lay.side.y()}}});
    auto r = withSheetJson(nd, size_t(si), sj);
    if (!r) return r.error();
    autoNameDrawing(*r, file, element);  // FX9-DRW-003
    if (auto s = commit(std::move(*r), "undo.addThreeViews", {}, ids[0]); !s) return s.error();
    return ids;
}

// ---------------------------------------------------------------- §5 sheet primitives

Result<drawing::SheetGraphics> Session::sheetGraphics(const std::string& sheetId) const {
    const auto& el = doc().active();
    if (el.isSpecification()) return specificationSheet(sheetId);  // core-api §4.1е
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr);
    return drawing::composeSheet(doc(), el, el.sheets[size_t(si)], models);
}

// Sheets of several drawings (core-api §4.7в, FX7-DRW-004, FX7-DRW-005).
Result<std::vector<drawing::SheetGraphics>> Session::drawingSheets(const std::vector<std::string>& drawings) {
    if (drawings.empty()) return Error("EXPORT_INVALID_OPTIONS").with("name", std::string("drawings")).with("actual", drawings);
    std::vector<drawing::SheetGraphics> out;
    for (const auto& id : drawings) {
        const int ei = doc().elementIndex(id);
        if (ei < 0) return notFound(id);
        if (doc().elements[size_t(ei)].isSpecification()) {  // all pages of a specification (FX8-DRW-005)
            const doc::Element& el = doc().elements[size_t(ei)];
            // The fastener rows (FX14-SPEC-001): the regeneration with this specification active computes its assembly.
            doc::Document sd = doc();
            sd.activeElement = id;
            auto spr = elementResult(sd);
            if (!spr) return spr.error();
            const regen::StatePtr ast = (*spr)->specAssembly;
            const auto content = drawing::specificationContent(doc(), el, specLinkedDocs(), ast ? &ast->connections : nullptr);
            const auto pages = drawing::specificationPages(content);
            for (size_t p = 0; p < pages.size(); ++p) out.push_back(drawing::composeSpecificationPage(doc(), el, content, pages, int(p)));
            continue;
        }
        if (!doc().elements[size_t(ei)].isDrawing()) return Error("API_NOT_DRAWING").with("id", id);
        // View models: of the published regeneration for the active drawing, else of a regeneration with this drawing
        // active (as meshReport); missing projections are computed synchronously (compute = true).
        doc::Document d = doc();
        d.activeElement = id;
        auto pr = elementResult(d);
        if (!pr) return pr.error();
        auto ms = std::make_shared<const SessionModels::Models>((*pr)->models);
        precomputeSheets(k_, projCache_, d, ms, selfPath(), nullptr, &links_);  // in parallel (FX8-DRW-001)
        SessionModels models(k_, ms, selfPath(), projCache_, true, nullptr, &links_);
        for (const auto& sh : d.active().sheets) out.push_back(drawing::composeSheet(d, d.active(), sh, models));
    }
    return out;
}

Result<std::map<std::string, std::string>> Session::titleBlockDefaults(const std::string& sheetId) const {
    const auto& el = doc().active();
    if (el.isSpecification()) return specificationTitleDefaults(sheetId);  // core-api §4.1е
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;  // models - from the last regeneration, as in sheetGraphics
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr);
    return drawing::titleBlockDefaults(doc(), el, el.sheets[size_t(si)], models);
}

namespace {

bool sameRequest(const SessionModels::Request& a, const SessionModels::Request& b) {
    return a.key && b.key && a.section == b.section && (!a.section || a.origin == b.origin) && *a.key == *b.key;
}

// Projections and sectionings of the sheets of drawing d (core-api.md §5.4 «Where computed», FX8-DRW-001): rounds of
// "compose with recording of cache misses → compute the distinct misses in parallel"; a round without misses ends it
// (a section view needs its sectioning first, its projection — in the next round). Not converged after
// DRAWING_PRECOMPUTE_ROUNDS — the rest is computed sequentially by the last compose. false — cancelled.
bool precomputeSheets(const kernel::Kernel& k, ProjectionCache& cache, const doc::Document& d,
                      const std::shared_ptr<const SessionModels::Models>& ms, const std::string& self, const Cancel* c,
                      const regen::LinkMap* links) {
    for (int round = 0; round < DRAWING_PRECOMPUTE_ROUNDS; ++round) {
        std::vector<SessionModels::Request> misses;
        SessionModels rec(k, ms, self, cache, false, c, links, &misses);
        for (const auto& sh : d.active().sheets) drawing::composeSheet(d, d.active(), sh, rec);
        if (cancelled(c)) return false;
        std::vector<SessionModels::Request> todo;
        for (auto& m : misses)
            if (std::none_of(todo.begin(), todo.end(), [&](const SessionModels::Request& t) { return sameRequest(t, m); }))
                todo.push_back(std::move(m));
        if (todo.empty()) return true;
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t i = next++; i < todo.size(); i = next++) {
                if (cancelled(c)) return;
                const auto& q = todo[i];
                if (q.section) SessionModels::computeSection(k, cache, q.bodies, q.view, q.origin, q.params, q.key, c);
                else SessionModels::computeProjection(k, cache, q.bodies, q.view, q.params, q.key, c);
            }
        };
        const size_t threads = std::min<size_t>(todo.size(), drawingThreads());
        std::vector<WorkerThread> pool;  // explicit stack size: projection recursion on macOS (base/thread.h)
        for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();
        if (cancelled(c)) return false;
    }
    SessionModels models(k, ms, self, cache, true, c, links);
    for (const auto& sh : d.active().sheets) drawing::composeSheet(d, d.active(), sh, models);
    return !cancelled(c);
}

}  // namespace

size_t drawingThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp<size_t>(hw == 0 ? 1 : hw, 1, DRAWING_PARALLEL_MAX);
}

bool Session::precomputeDrawing(const doc::Document& d, const regen::PartResult& r, const std::string& self, const Cancel* c) {
    return precomputeSheets(k_, projCache_, d, std::make_shared<const SessionModels::Models>(r.models), self, c, nullptr);
}

std::map<std::string, int> Session::drawingProjectionStats() const { return projCache_.stats(); }

// ---------------------------------------------------------------- §4.1г, §5 dimensions (S3-DRW-006, drawing.md §6а)

namespace {

// Sheet and dimension index by its ID in the active drawing; sheet < 0 - none.
Located locateDimension(const doc::Element& el, const std::string& id) {
    for (size_t si = 0; si < el.sheets.size(); ++si)
        if (const int ai = el.sheets[si].dimensionIndex(id); ai >= 0) return {int(si), ai};
    return {};
}

// Schema error in the sheet → path inside the definition of dimension ai.
Error dimensionDefinitionError(Error e, size_t ai) {
    if (e.path) {
        const std::string ap = "/dimensions/" + std::to_string(ai);
        e.path = e.path->rfind(ap, 0) == 0 ? std::optional<std::string>(e.path->substr(ap.size())) : std::nullopt;
        if (e.path && e.path->empty()) e.path.reset();
    }
    return e;
}

}  // namespace

Result<std::string> Session::addDimension(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    if (auto s = checkNoIdName(def); !s) return s.error();
    doc::Document nd = doc();
    Json aj = Json::object();
    aj["id"] = doc::allocate(nd, doc::IdKind::Dimension);
    mergePatch(aj, def);
    const std::string id = aj["id"];
    Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
    if (!sj.contains("dimensions")) sj["dimensions"] = Json::array();
    sj["dimensions"].push_back(aj);
    auto r = withSheetJson(nd, size_t(si), sj);
    if (!r) return dimensionDefinitionError(r.error(), sj["dimensions"].size() - 1);
    const std::string view = aj.value("view", std::string());
    if (doc().active().sheets[size_t(si)].viewIndex(view) < 0) return notFound(view);
    if (auto s = commit(std::move(*r), "undo.addDimension", {}, id); !s) return s.error();
    return id;
}

Result<std::string> Session::addDimension(const std::string& sheetId, const doc::SheetDimension& a) {
    Json def = doc::dimensionToJsonValue(a);
    def.erase("id");  // the command assigns it
    return addDimension(sheetId, def);
}

// Dimensions from the model (core-api §4.1г, drawing.md §6а.10, FX1-DRW-003): in one undo step.
Result<std::vector<std::string>> Session::addModelDimensions(const std::string& sheetId, const std::vector<std::string>& views,
                                                             std::vector<drawing::UnplacedDimension>* unplaced) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    loadLinks();  // documents of the views' linked files (dimensions from the model of other files)
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr, &links_);
    const doc::Sheet& sheet = doc().active().sheets[size_t(si)];
    for (const auto& v : views)
        if (sheet.viewIndex(v) < 0) return notFound(v);
    const std::set<std::string> only(views.begin(), views.end());
    const auto dims = drawing::modelDimensions(doc(), doc().active(), sheet, models, views.empty() ? nullptr : &only, unplaced);
    std::vector<std::string> ids;
    if (dims.empty()) return ids;
    doc::Document nd = doc();
    doc::Sheet& sh = nd.active().sheets[size_t(si)];
    for (auto a : dims) {
        a.id = doc::allocate(nd, doc::IdKind::Dimension);
        ids.push_back(a.id);
        sh.dimensions.push_back(std::move(a));
    }
    auto r = withSheetJson(nd, size_t(si), doc::sheetToJsonValue(sh));  // via the schema, as addDimension
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.modelDimensions", {}, ids.front()); !s) return s.error();
    return ids;
}

// Model dimensions not yet on the drawing (FX7-DRW-003): the result of addModelDimensions with primitives, no document change.
Result<std::vector<drawing::DimensionPreview>> Session::undimensioned(const std::string& sheetId, const std::vector<std::string>& views) const {
    const auto& el = doc().active();
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr, &links_);
    const doc::Sheet& sheet = el.sheets[size_t(si)];
    const std::string font = drawing::drawingFont(el);
    const drawing::DrawingFontScope fontScope(font);
    // The selected views first (FX14-DRW-001): as addModelDimensions with them; IDs not on the sheet are skipped.
    std::set<std::string> only;
    for (const auto& v : views)
        if (sheet.viewIndex(v) >= 0) only.insert(v);
    std::vector<drawing::DimensionPreview> out;
    for (const auto& a : drawing::modelDimensions(doc(), el, sheet, models, only.empty() ? nullptr : &only)) {
        auto r = drawing::previewDimension(doc(), el, sheet, a, std::nullopt, models);
        if (!r) continue;
        drawing::applyDrawingFont(r->primitives, font);
        out.push_back(std::move(*r));
    }
    return out;
}

Status Session::updateDimension(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateDimension(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    if (auto s = checkNoIdName(def); !s) return s;
    Json sj = doc::sheetToJsonValue(doc().active().sheets[size_t(at.sheet)]);
    Json& aj = sj["dimensions"][size_t(at.view)];
    mergePatch(aj, def);
    const std::string view = aj.value("view", std::string());
    auto r = withSheetJson(doc(), size_t(at.sheet), sj);
    if (!r) return dimensionDefinitionError(r.error(), size_t(at.view));
    if (doc().active().sheets[size_t(at.sheet)].viewIndex(view) < 0) return notFound(view);
    return commit(std::move(*r), "undo.updateDimension", {}, id);
}

Status Session::deleteDimension(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateDimension(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    doc::Document nd = doc();
    auto& list = nd.active().sheets[size_t(at.sheet)].dimensions;
    list.erase(list.begin() + at.view);
    return commit(std::move(nd), "undo.deleteDimension", {}, id);
}

// ---------------------------------------------------------------- §4.1г texts of sheets (FX7-DRW-006, drawing.md §6.23)

namespace {
Located locateText(const doc::Element& el, const std::string& id) {
    for (size_t si = 0; si < el.sheets.size(); ++si)
        if (const int ti = el.sheets[si].textIndex(id); ti >= 0) return {int(si), ti};
    return {};
}

Error textDefinitionError(Error e, size_t ti) {
    if (e.path) {
        const std::string tp = "/texts/" + std::to_string(ti);
        e.path = e.path->rfind(tp, 0) == 0 ? std::optional<std::string>(e.path->substr(tp.size())) : std::nullopt;
        if (e.path && e.path->empty()) e.path.reset();
    }
    return e;
}
}  // namespace

Result<std::string> Session::addSheetText(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    if (auto s = checkNoIdName(def); !s) return s.error();
    doc::Document nd = doc();
    Json tj = Json::object();
    tj["id"] = doc::allocate(nd, doc::IdKind::Text);
    mergePatch(tj, def);
    const std::string id = tj["id"];
    Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
    if (!sj.contains("texts")) sj["texts"] = Json::array();
    sj["texts"].push_back(tj);
    auto r = withSheetJson(nd, size_t(si), sj);
    if (!r) return textDefinitionError(r.error(), sj["texts"].size() - 1);
    if (auto s = commit(std::move(*r), "undo.addSheetText", {}, id); !s) return s.error();
    return id;
}

Status Session::updateSheetText(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateText(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    if (auto s = checkNoIdName(def); !s) return s;
    Json sj = doc::sheetToJsonValue(doc().active().sheets[size_t(at.sheet)]);
    mergePatch(sj["texts"][size_t(at.view)], def);
    auto r = withSheetJson(doc(), size_t(at.sheet), sj);
    if (!r) return textDefinitionError(r.error(), size_t(at.view));
    return commit(std::move(*r), "undo.updateSheetText", {}, id);
}

Status Session::deleteSheetText(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateText(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    doc::Document nd = doc();
    auto& list = nd.active().sheets[size_t(at.sheet)].texts;
    list.erase(list.begin() + at.view);
    return commit(std::move(nd), "undo.deleteSheetText", {}, id);
}

// ---------------------------------------------------------------- §4.1г constructions (FX14-DRW-006, drawing.md §6.29)

namespace {
Located locateConstruction(const doc::Element& el, const std::string& id) {
    for (size_t si = 0; si < el.sheets.size(); ++si)
        if (const int ci = el.sheets[si].constructionIndex(id); ci >= 0) return {int(si), ci};
    return {};
}

Error constructionDefinitionError(Error e, size_t ci) {
    if (e.path) {
        const std::string cp = "/constructions/" + std::to_string(ci);
        e.path = e.path->rfind(cp, 0) == 0 ? std::optional<std::string>(e.path->substr(cp.size())) : std::nullopt;
        if (e.path && e.path->empty()) e.path.reset();
    }
    return e;
}
}  // namespace

Result<std::string> Session::addConstruction(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    if (auto s = checkNoIdName(def); !s) return s.error();
    doc::Document nd = doc();
    Json cj = Json::object();
    cj["id"] = doc::allocate(nd, doc::IdKind::Construction);
    mergePatch(cj, def);
    const std::string id = cj["id"];
    Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
    if (!sj.contains("constructions")) sj["constructions"] = Json::array();
    sj["constructions"].push_back(cj);
    auto r = withSheetJson(nd, size_t(si), sj);
    if (!r) return constructionDefinitionError(r.error(), sj["constructions"].size() - 1);
    const std::string view = cj.value("view", std::string());
    if (doc().active().sheets[size_t(si)].viewIndex(view) < 0) return notFound(view);
    if (auto s = commit(std::move(*r), "undo.addConstruction", {}, id); !s) return s.error();
    return id;
}

Status Session::updateConstruction(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateConstruction(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    if (auto s = checkNoIdName(def); !s) return s;
    Json sj = doc::sheetToJsonValue(doc().active().sheets[size_t(at.sheet)]);
    Json& cj = sj["constructions"][size_t(at.view)];
    mergePatch(cj, def);
    const std::string view = cj.value("view", std::string());
    auto r = withSheetJson(doc(), size_t(at.sheet), sj);
    if (!r) return constructionDefinitionError(r.error(), size_t(at.view));
    if (doc().active().sheets[size_t(at.sheet)].viewIndex(view) < 0) return notFound(view);
    return commit(std::move(*r), "undo.updateConstruction", {}, id);
}

Status Session::deleteConstruction(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateConstruction(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    doc::Document nd = doc();
    eraseConstructions(nd.active().sheets[size_t(at.sheet)], [&](const doc::SheetConstruction& c) { return c.id == id; });
    return commit(std::move(nd), "undo.deleteConstruction", {}, id);
}

Result<drawing::ConstructionPreview> Session::previewConstruction(const std::string& sheetId, const doc::SheetConstruction& c) const {
    const auto& el = doc().active();
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr);
    return drawing::previewConstruction(doc(), el, el.sheets[size_t(si)], c, models);
}

// ---------------------------------------------------------------- §4.1г sheet marks (FX9-DRW-008, drawing.md §6.25)

namespace {
Located locateMark(const doc::Element& el, const std::string& id) {
    for (size_t si = 0; si < el.sheets.size(); ++si)
        if (const int mi = el.sheets[si].markIndex(id); mi >= 0) return {int(si), mi};
    return {};
}
}  // namespace

// The drawing name by the model of its first view (drawing.md §2.6, FX9-DRW-003): in the same step as the view.
void Session::autoNameDrawing(doc::Document& nd, const std::string& file, const std::string& element) const {
    const doc::Element& before = doc().active();
    if (!before.isDrawing() || before.nameManual) return;
    if (std::any_of(before.sheets.begin(), before.sheets.end(), [](const doc::Sheet& sh) { return !sh.views.empty(); })) return;
    std::string model;
    if (file.empty()) {
        const int ei = nd.elementIndex(element);
        if (ei >= 0) model = nd.elements[size_t(ei)].name;
    } else {
        const std::string abs = regen::linkPath(regen::parentDir(selfPath()), file);
        if (auto it = links_.find(abs); it != links_.end() && it->second.doc) {
            const auto* ld = static_cast<const doc::Document*>(it->second.doc);
            const int ei = ld->elementIndex(element);
            if (ei >= 0) model = ld->elements[size_t(ei)].name;
        }
        if (model.empty()) {  // the file name without the path and extension
            model = file.substr(file.find_last_of("/\\") == std::string::npos ? 0 : file.find_last_of("/\\") + 1);
            if (const size_t dot = model.rfind('.'); dot != std::string::npos && dot > 0) model.resize(dot);
        }
    }
    if (model.empty()) return;
    std::string base = drawingNameTemplate_;
    if (const size_t p = base.find("{name}"); p != std::string::npos) base.replace(p, 6, model);
    doc::Element& el = nd.active();
    auto taken = [&](const std::string& n) {
        return std::any_of(nd.elements.begin(), nd.elements.end(), [&](const doc::Element& e) { return e.id != el.id && e.name == n; });
    };
    std::string name = base;
    for (int n = 2; taken(name); ++n) name = base + " " + std::to_string(n);
    el.name = name;
}

Result<std::vector<std::string>> Session::addModelMarks(const std::string& sheetId, const std::vector<std::string>& views) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr, &links_);
    const auto marks = drawing::modelMarks(doc(), doc().active(), doc().active().sheets[size_t(si)], models, views);
    std::vector<std::string> ids;
    if (marks.empty()) return ids;
    doc::Document nd = doc();
    doc::Sheet& sh = nd.active().sheets[size_t(si)];
    for (auto m : marks) {
        m.id = doc::allocate(nd, doc::IdKind::Mark);
        ids.push_back(m.id);
        sh.marks.push_back(std::move(m));
    }
    auto r = withSheetJson(nd, size_t(si), doc::sheetToJsonValue(sh));  // via the schema, as addDimension
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.modelMarks", {}, ids.front()); !s) return s.error();
    return ids;
}

Result<std::vector<drawing::MarkPreview>> Session::unplacedMarks(const std::string& sheetId, const std::vector<std::string>& views) const {
    const auto& el = doc().active();
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr, &links_);
    std::vector<drawing::MarkPreview> out;
    auto marks = drawing::modelMarks(doc(), el, el.sheets[size_t(si)], models, views);
    // Item numbers not on the drawing (FX14-DRW-004, drawing.md §6.27): blue only, after the command's marks.
    for (auto& b : drawing::balloonMarks(doc(), el, el.sheets[size_t(si)], models, views)) marks.push_back(std::move(b));
    if (marks.empty()) return out;
    // The sheet with the marks appended under temporary IDs: the images are those the marks would have when placed.
    doc::Element tmp = el;
    doc::Sheet& sh = tmp.sheets[size_t(si)];
    for (size_t i = 0; i < marks.size(); ++i) {
        doc::SheetMark m = marks[i];
        m.id = "unplaced:" + std::to_string(i);
        sh.marks.push_back(std::move(m));
    }
    const auto g = drawing::composeSheet(doc(), tmp, sh, models);
    for (size_t i = 0; i < marks.size(); ++i) {
        const std::string id = "unplaced:" + std::to_string(i);
        const auto it = std::find_if(g.marks.begin(), g.marks.end(), [&](const drawing::MarkPlacement& p) { return p.mark == id; });
        if (it == g.marks.end()) continue;  // not drawn (no image of its face on the view)
        drawing::MarkPreview p{marks[i], {}};
        for (const auto& pr : g.primitives)
            if (std::find(it->roles.begin(), it->roles.end(), pr.role) != it->roles.end()) p.primitives.push_back(pr);
        if (!p.primitives.empty()) out.push_back(std::move(p));
    }
    return out;
}

Result<std::string> Session::addModelMark(const std::string& sheetId, const doc::SheetMark& mark) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr, &links_);
    const doc::Sheet& sheet = doc().active().sheets[size_t(si)];
    const doc::DismissedMark key = drawing::markKey(sheet, mark);
    // Only a mark the command would place now (the same key); its own record — as the command builds it with the
    // mark's view selected (FX14-DRW-005: a blue mark goes onto the view where it is shown).
    // An item number (FX14-DRW-004) — among the item numbers not on the drawing, with its view selected.
    const auto marks = mark.kind == "balloon" ? drawing::balloonMarks(doc(), doc().active(), sheet, models, {mark.view})
                                              : drawing::modelMarks(doc(), doc().active(), sheet, models, {mark.view});
    const auto it = std::find_if(marks.begin(), marks.end(), [&](const doc::SheetMark& m) { return drawing::markKey(sheet, m) == key; });
    if (it == marks.end()) return Error("API_INVALID_DEFINITION").with("detail", std::string("mark")).at("/source");
    doc::Document nd = doc();
    doc::Sheet& sh = nd.active().sheets[size_t(si)];
    doc::SheetMark m = *it;
    m.id = doc::allocate(nd, doc::IdKind::Mark);
    const std::string id = m.id;
    sh.marks.push_back(std::move(m));
    auto r = withSheetJson(nd, size_t(si), doc::sheetToJsonValue(sh));
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.modelMarks", {}, id); !s) return s.error();
    return id;
}

Status Session::setDrawingRestRoughness(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    auto bad = [](const char* key) { return Error("API_INVALID_DEFINITION").with("detail", std::string(key)).at(std::string("/") + key); };
    if (!def.is_null() && (!def.is_object() || def.contains("parameter") != def.contains("value"))) return bad("def");
    doc::Document nd = doc();
    doc::Element& el = nd.active();
    doc::Sheet& sh = el.sheets[size_t(si)];
    auto it = std::find_if(sh.marks.begin(), sh.marks.end(), [](const doc::SheetMark& m) { return m.kind == "restRoughness"; });
    std::string label;
    if (def.is_null()) {
        if (it == sh.marks.end() || !it->local) return okStatus();  // nothing set in the drawing — no step
        // Back to the model value if the model of the view has one; otherwise the sign goes.
        const auto t = drawing::viewTriad(sh, it->view);
        const int ei = t && t->base->refFile.empty() ? nd.elementIndex(t->base->refElement) : -1;
        if (ei >= 0 && nd.elements[size_t(ei)].restRoughness) {
            it->local = false;
            it->def.reset();
            label = "undo.updateMark";
        } else {
            sh.marks.erase(it);
            label = "undo.deleteMark";
        }
    } else if (it != sh.marks.end()) {
        if (it->local && it->def && *it->def == doc::canonicalRestRoughness(def)) return okStatus();  // unchanged
        it->local = true;
        it->def = doc::canonicalRestRoughness(def);
        label = "undo.updateMark";
    } else {
        // A new sign: the view — the first view of the sheet with manufacturing information (its main view's model).
        std::string view;
        for (const auto& v : sh.views)
            if (v.pmi && view.empty()) view = v.id;
        if (view.empty()) return bad("view");
        doc::SheetMark m;
        m.kind = "restRoughness";
        m.view = view;
        m.local = true;
        m.def = doc::canonicalRestRoughness(def);
        m.id = doc::allocate(nd, doc::IdKind::Mark);
        sh.marks.push_back(std::move(m));
        label = "undo.addMark";
    }
    auto r = withSheetJson(nd, size_t(si), doc::sheetToJsonValue(sh));
    if (!r) return r.error();
    r->counters = nd.counters;
    return commit(std::move(*r), label, {}, sheetId);
}

Result<std::string> Session::addMark(const std::string& sheetId, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const int si = doc().active().sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    if (auto s = checkNoIdName(def); !s) return s.error();
    // Only an annotation set in the drawing (FX9-DRW-009, drawing.md §6.25): roughness, geometric tolerance or datum on a face.
    auto bad = [](const char* key) { return Error("API_INVALID_DEFINITION").with("detail", std::string(key)).at(std::string("/") + key); };
    // An item number (balloon, FX14-DRW-002) or a parts list (FX14-DRW-003), drawing.md §6.27, §6.28.
    if (def.is_object() && (def.value("kind", std::string()) == "balloon" || def.value("kind", std::string()) == "partsList")) {
        const bool balloon = def["kind"] == "balloon";
        for (auto it = def.begin(); it != def.end(); ++it)
            if (it.key() != "kind" && it.key() != "position" && !(balloon && (it.key() == "view" || it.key() == "source"))) return bad(it.key().c_str());
        const doc::Sheet& sh0 = doc().active().sheets[size_t(si)];
        if (balloon) {
            if (!def.contains("source") || !def["source"].is_string() || def["source"].get<std::string>().empty()) return bad("source");
            const auto t = drawing::viewTriad(sh0, def.value("view", std::string()));
            if (sh0.viewIndex(def.value("view", std::string())) < 0 || !t || !t->base->refFile.empty()) return bad("view");
            const int ei = doc().elementIndex(t->base->refElement);
            if (ei < 0 || !doc().elements[size_t(ei)].isAssembly()) return bad("view");
        }
        doc::Document nd = doc();
        Json m = def;
        m["id"] = doc::allocate(nd, doc::IdKind::Mark);
        const std::string id = m["id"].get<std::string>();
        Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
        if (!sj.contains("marks")) sj["marks"] = Json::array();
        sj["marks"].push_back(m);
        auto r = withSheetJson(nd, size_t(si), sj);
        if (!r) return r.error();
        r->counters = nd.counters;
        if (auto s = commit(std::move(*r), "undo.addMark", {}, id); !s) return s.error();
        return id;
    }
    if (!def.is_object() || def.value("kind", std::string()) != "annotation") return bad("kind");
    if (def.contains("source")) return bad("source");
    if (!def.contains("def") || !def["def"].is_object()) return bad("def");
    const std::string type = def["def"].value("type", std::string());
    if ((type != "roughness" && type != "geometric" && type != "datum") || !def["def"].contains("face") || !def["def"]["face"].is_string())
        return bad("def");
    if (doc().active().sheets[size_t(si)].viewIndex(def.value("view", std::string())) < 0) return bad("view");
    doc::Document nd = doc();
    Json m = def;
    m["local"] = true;
    m["id"] = doc::allocate(nd, doc::IdKind::Mark);
    const std::string id = m["id"].get<std::string>();
    Json sj = doc::sheetToJsonValue(nd.active().sheets[size_t(si)]);
    if (!sj.contains("marks")) sj["marks"] = Json::array();
    sj["marks"].push_back(m);
    auto r = withSheetJson(nd, size_t(si), sj);
    if (!r) return r.error();
    r->counters = nd.counters;
    if (auto s = commit(std::move(*r), "undo.addMark", {}, id); !s) return s.error();
    return id;
}

Status Session::updateMark(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateMark(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    if (auto s = checkNoIdName(def); !s) return s;
    for (const char* key : {"kind", "view", "source"})
        if (def.is_object() && def.contains(key)) return Error("API_INVALID_DEFINITION").with("detail", std::string(key)).at(std::string("/") + key);
    Json patch = def;
    // «Set in drawing» (FX9-DRW-009): local on — the values are copied from the model annotation (unless given); off —
    // the values from the model again (only a mark of a model object).
    {
        const doc::Sheet& sh = doc().active().sheets[size_t(at.sheet)];
        const doc::SheetMark& m = sh.marks[size_t(at.view)];
        if (patch.is_object() && patch.contains("local")) {
            const bool on = patch["local"].is_boolean() && patch["local"].get<bool>();
            if (!on) {
                if (m.source.empty()) return Error("API_INVALID_DEFINITION").with("detail", std::string("local")).at("/local");
                patch["local"] = nullptr;
                patch["def"] = nullptr;
            } else if (!m.local && !patch.contains("def")) {
                const doc::Json* a = nullptr;
                if (const auto t = drawing::viewTriad(sh, m.view); t && t->base->refFile.empty())
                    if (const int ei = doc().elementIndex(t->base->refElement); ei >= 0)
                        for (const auto& x : doc().elements[size_t(ei)].annotations)
                            if (x.value("id", std::string()) == m.source) a = &x;
                if (!a || m.kind != "annotation") return Error("API_INVALID_DEFINITION").with("detail", std::string("local")).at("/local");
                Json copy = *a;
                copy.erase("id");
                patch["def"] = copy;
            }
        } else if (patch.is_object() && patch.contains("def") && !m.local) {
            return Error("API_INVALID_DEFINITION").with("detail", std::string("def")).at("/def");
        }
    }
    Json sj = doc::sheetToJsonValue(doc().active().sheets[size_t(at.sheet)]);
    mergePatch(sj["marks"][size_t(at.view)], patch);
    auto r = withSheetJson(doc(), size_t(at.sheet), sj);
    if (!r) {
        Error e = r.error();
        if (e.path) {
            const std::string mp = "/marks/" + std::to_string(at.view);
            e.path = e.path->rfind(mp, 0) == 0 ? std::optional<std::string>(e.path->substr(mp.size())) : std::nullopt;
            if (e.path && e.path->empty()) e.path.reset();
        }
        return e;
    }
    return commit(std::move(*r), "undo.updateMark", {}, id);
}

Status Session::deleteMark(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const Located at = locateMark(doc().active(), id);
    if (at.sheet < 0) return notFound(id);
    doc::Document nd = doc();
    doc::Element& el = nd.active();
    auto& list = el.sheets[size_t(at.sheet)].marks;
    // Nothing is remembered (FX12-DRW-005): «Place all additional information» places a deleted mark again.
    list.erase(list.begin() + at.view);
    return commit(std::move(nd), "undo.deleteMark", {}, id);
}

Result<std::vector<drawing::ViewEntity>> Session::viewEntities(const std::string& sheetId, const std::string& viewId) const {
    const auto& el = doc().active();
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr);
    return drawing::viewEntities(doc(), el, el.sheets[size_t(si)], viewId, models);
}

Result<drawing::DimensionPreview> Session::previewDimension(const std::string& sheetId, const doc::SheetDimension& a,
                                                            const std::optional<Point2>& cursor) const {
    const auto& el = doc().active();
    if (!el.isDrawing()) return Error("API_NOT_DRAWING").with("id", el.id);
    const int si = el.sheetIndex(sheetId);
    if (si < 0) return notFound(sheetId);
    std::shared_ptr<const SessionModels::Models> ms;
    {
        std::lock_guard lk(mu_);
        ms = drawingModels_;
    }
    SessionModels models(k_, ms, selfPath(), projCache_, false, nullptr);
    // Drawing font per the standard (drawing.md §3.2): text width during layout and the font of preview text.
    const std::string font = drawing::drawingFont(el);
    const drawing::DrawingFontScope fontScope(font);
    auto r = drawing::previewDimension(doc(), el, el.sheets[size_t(si)], a, cursor, models);
    if (r) drawing::applyDrawingFont(r->primitives, font);
    return r;
}

}  // namespace construe::api
