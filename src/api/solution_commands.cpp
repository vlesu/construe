// Library of standard methods in the session (core-api.md §4.1и, solutions.md; S6-SOL-001…008): the catalog of solution
// packages in the library folders, the wizard plan with suitable variants, applying a solution as a connection feature,
// editing it and its report.
#include "api/errors.h"
#include "base/files.h"
#include "api/feature_commands.h"
#include "api/owned_cuts.h"
#include "api/session.h"
#include "io/stl.h"
#include "regen/hole_tables.h"
#include "regen/naming.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace construe::api {

namespace {

// Table of a library package from the catalog (solutions.md §3.3): the same values as a document copy gives.
solutions::TableData packageTable(const library::Package& p) {
    solutions::TableData out;
    out.columns = p.sizes.columns;
    for (const auto& r : p.sizes.rows) {
        const expr::VariableTable vars = library::rowVariables(p, r.name);
        solutions::TableRow row;
        row.name = r.name;
        row.texts = r.values;
        for (const auto& c : out.columns) {
            const auto* e = vars.find(c);
            row.values.push_back(e ? e->value : Result<expr::Quantity>(Error("EXPR_UNKNOWN_NAME")));
        }
        row.designation = library::rowDesignation(p, r.name);
        out.rows.push_back(std::move(row));
    }
    return out;
}

// Tables of a catalog solution: own files, built-in, library packages of the catalog; cached for repeated plans.
solutions::TableProvider catalogTables(const solutions::Solution& s, std::shared_ptr<const library::Catalog> cat) {
    auto def = std::make_shared<solutions::Definition>(solutions::parseDefinition(s.definition));
    auto files = std::make_shared<std::map<std::string, solutions::DataTable>>(s.files);
    auto cache = std::make_shared<std::map<std::string, std::optional<solutions::TableData>>>();
    return [def, files, cat, cache](const std::string& id) -> std::optional<solutions::TableData> {
        if (auto it = cache->find(id); it != cache->end()) return it->second;
        std::optional<solutions::TableData> out;
        if (const solutions::TableDef* t = def->table(id)) {
            if (!t->builtin.empty()) out = solutions::holesTable(regen::holeTables());
            else if (!t->file.empty()) {
                if (auto it = files->find(id); it != files->end()) out = solutions::fileTable(it->second);
            } else if (const library::CatalogItem* item = cat->find(t->library)) {
                out = packageTable(item->package);
            }
        }
        (*cache)[id] = out;
        return out;
    };
}

// Document tables with a cache (editing an existing connection).
solutions::TableProvider cached(solutions::TableProvider p) {
    auto cache = std::make_shared<std::map<std::string, std::optional<solutions::TableData>>>();
    return [p, cache](const std::string& id) -> std::optional<solutions::TableData> {
        if (auto it = cache->find(id); it != cache->end()) return it->second;
        return (*cache)[id] = p(id);
    };
}

// Library tables of the catalog for the tables a document provider lacks (editing, solutions.md §5.2): a changed choice
// may need a library package that is not embedded yet.
solutions::TableProvider withCatalogFallback(solutions::TableProvider p, const doc::Json& definition, std::shared_ptr<const library::Catalog> cat) {
    auto def = std::make_shared<solutions::Definition>(solutions::parseDefinition(definition));
    return [p, def, cat](const std::string& id) -> std::optional<solutions::TableData> {
        if (auto t = p(id)) return t;
        if (const solutions::TableDef* t = def->table(id); t && !t->library.empty())
            if (const library::CatalogItem* item = cat->find(t->library)) return packageTable(item->package);
        return std::nullopt;
    };
}

// A provider that records the tables a plan reads (only those library packages are embedded, solutions.md §5.2).
solutions::TableProvider tracking(solutions::TableProvider p, std::shared_ptr<std::set<std::string>> used) {
    return [p, used](const std::string& id) {
        used->insert(id);
        return p(id);
    };
}

// Embeds the library packages of the used library tables of the solution that tableMap lacks (solutions.md §5.2).
void embedUsedTables(doc::Document& nd, const doc::Json& definition, const std::set<std::string>& used, doc::Json& tableMap,
                     const library::Catalog& cat) {
    for (const auto& t : solutions::parseDefinition(definition).tables) {
        if (t.library.empty() || !used.count(t.id) || tableMap.contains(t.id)) continue;
        const library::CatalogItem* li = cat.find(t.library);
        if (!li) continue;
        doc::LibraryPart lp = library::embed(li->package);
        if (!nd.libraryPart(lp.id)) nd.libraryParts.push_back(lp);
        tableMap[t.id] = lp.id;
    }
}

// A list of points [[x, y, z], …] (≥ 1) for the point input (solutions.md §7.12, FX8-BLT-002).
bool isPointList(const Json& v) {
    if (!v.is_array() || v.empty()) return false;
    for (const auto& p : v)
        if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number()) return false;
    return true;
}

// The point input as one point for a plan (§7.12): a list — its first point; a sketch — its first point in v.
Json firstPointInputs(const solutions::Definition& def, const Json& inputs, const doc::Element& el, const regen::PartState& v) {
    if (!def.frameAxis.empty() && inputs.contains(def.frameAxis) && regen::isAxisList(inputs[def.frameAxis])) {  // §7.11
        Json out = inputs;
        out[def.frameAxis] = inputs[def.frameAxis][0];
        return out;
    }
    if (def.framePoint.empty() || !inputs.contains(def.framePoint)) return inputs;
    const Json& p = inputs[def.framePoint];
    Json out = inputs;
    if (isPointList(p)) out[def.framePoint] = p[0];
    else if (auto pts = regen::connectionSketchPoints(el, v, def, inputs); pts && *pts) out = regen::inputsAtPoint(def, inputs, (**pts)[0]);
    return out;
}

std::string defaultConnectionName(const doc::Element& el, const std::string& title) {
    for (int n = 1;; ++n) {
        const std::string name = title + std::to_string(n);
        if (std::none_of(el.features.begin(), el.features.end(), [&](const doc::FeaturePtr& f) { return f->name == name; })) return name;
    }
}

}  // namespace

std::shared_ptr<const solutions::Catalog> Session::solutionCatalog() const {
    if (!solutionCatalog_) solutionCatalog_ = std::make_shared<const solutions::Catalog>(solutions::scan(libraryDirs_));
    return solutionCatalog_;
}

Result<Session::SolutionPlan> Session::solutionPlan(const std::string& source, const Json& inputs, const Json& choices, bool withOptions,
                                                    bool lazy) const {
    const doc::Element& el = doc().active();
    if (!el.isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", el.id);
    if (!inputs.is_object()) return badArg("inputs");
    if (!choices.is_object()) return badArg("choices");
    doc::Json definition;
    solutions::TableProvider tables;
    std::map<std::string, std::string> libraries;
    StateRef ref = StateRef::final();
    if (auto s = solutionSource(source, definition, tables, libraries, ref); !s) return s.error();
    return planOf(definition, tables, libraries, ref, inputs, choices, withOptions, lazy);
}

Result<Json> Session::solutionChoicesAfter(const std::string& source, const Json& choices, const std::string& changed) const {
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    if (!choices.is_object()) return badArg("choices");
    doc::Json definition;
    solutions::TableProvider tables;
    std::map<std::string, std::string> libraries;
    StateRef ref = StateRef::final();
    if (auto s = solutionSource(source, definition, tables, libraries, ref); !s) return s.error();
    const solutions::Definition def = solutions::parseDefinition(definition);
    if (!def.choice(changed)) return badArg("changed");
    return solutions::resetDependentChoices(def, choices, changed, tables);
}

Status Session::solutionSource(const std::string& source, doc::Json& definition, solutions::TableProvider& tables,
                               std::map<std::string, std::string>& libraries, StateRef& ref) const {
    const doc::Element& el = doc().active();
    if (const doc::FeaturePtr f = el.find(source); f && f->type == doc::FeatureType::Connection) {
        const doc::Json& d = f->pattern()->def;
        const doc::SolutionCopy* copy = doc().solution(d["solution"].get<std::string>());
        if (!copy) return notFound(d["solution"].get<std::string>());
        definition = copy->definition;
        const doc::Json tableMap = d.contains("tables") ? d["tables"] : doc::Json::object();
        tables = cached(withCatalogFallback(regen::connectionTables(doc(), *copy, tableMap), copy->definition, libraryCatalog()));
        for (const auto& [k, v] : tableMap.items()) libraries[k] = v.get<std::string>();
        const int i = el.indexOf(source);
        ref = i == 0 ? StateRef::initial() : StateRef::after(el.features[size_t(i - 1)]->id);
    } else {
        const solutions::CatalogItem* item = solutionCatalog()->find(source);
        if (!item) return notFound(source);
        definition = item->solution.definition;
        tables = catalogTables(item->solution, libraryCatalog());
    }
    return okStatus();
}

Result<Session::SolutionPlan> Session::planOf(const Json& definition, const solutions::TableProvider& tables,
                                              const std::map<std::string, std::string>& libraries, StateRef ref, const Json& inputsIn,
                                              const Json& choices, bool withOptions, bool lazy) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const solutions::Definition def = solutions::parseDefinition(definition);
    SolutionPlan out;
    // Several points (§7.12): the plan of the first point.
    const Json inputs = firstPointInputs(def, inputsIn, doc().active(), **st);
    // Pack measurements are kept for the state (FX8-BLT-001): a repeated plan and the variants reuse them.
    std::lock_guard<std::mutex> lock(packMu_);
    if (packState_ != *st) {
        packState_ = *st;
        packCache_ = regen::PackCache{};
    }
    packCache_.measure = true;
    const regen::ConnectionPlan plan = regen::planConnection(k_, def, tables, inputs, choices, **st, nullptr, &packCache_);
    out.error = plan.plan.error;
    out.report = regen::connectionReport(plan, libraries);
    const auto values = solutions::choiceValues(def, choices);
    for (const auto& c : def.choices) {
        out.values[c.id] = values.at(c.id);
        if (out.values[c.id].is_null() && c.type == "value") {  // the first cell — as in evaluation (§6.4)
            const auto opts = solutions::choiceOptions(def, c, values, tables);
            if (!opts.empty()) out.values[c.id] = opts.front().first;
        }
    }
    if (!withOptions) return out;
    // Suitable variants (§8): each variant of a choice substituted, the rest as is; lazy — a variant that needs a new
    // pack measurement (another probe radius) is not measured and counts as suitable (unchecked) — checked when chosen.
    packCache_.measure = !lazy;
    std::map<std::string, doc::Json> current = out.values;
    for (const auto& c : def.choices) {
        if (c.type == "length") continue;
        auto opts = solutions::choiceOptions(def, c, current, tables);
        out.options[c.id] = opts;
        auto& ok = out.suitable[c.id];
        // The type of the part (the fastener, §8, FX13-JNT-003): every variant is offered whatever the size — choosing
        // one moves the size to an available one (resetDependentChoices).
        if (solutions::isTypeChoice(def, c)) {
            for (const auto& [value, label] : opts) ok.push_back(value);
            continue;
        }
        for (const auto& [value, label] : opts) {
            doc::Json trial = choices;
            for (const auto& [k, v] : current) trial[k] = v;
            trial[c.id] = c.type == "bool" ? doc::Json(value == "true") : doc::Json(value);
            const auto p = regen::planConnection(k_, def, tables, inputs, trial, **st, nullptr, &packCache_);
            if (!p.plan.error) ok.push_back(value);
            else if (p.plan.error->code == "SOL_PACK_UNMEASURED") ok.push_back(value), out.unchecked[c.id].push_back(value);
        }
    }
    return out;
}

Result<Session::SolutionPlan> Session::solutionDraftPlan(const Json& definition, const Json& inputs, const Json& choices) const {
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    if (!definition.is_object()) return badArg("definition");
    if (!inputs.is_object()) return badArg("inputs");
    if (!choices.is_object()) return badArg("choices");
    if (auto s = solutions::checkDefinition(definition); !s) return s.error();
    solutions::Solution draft;
    draft.definition = definition;
    return planOf(definition, catalogTables(draft, libraryCatalog()), {}, StateRef::final(), inputs, choices, false);
}

Result<std::string> Session::saveSolution(const std::string& parent, const std::string& folder, const Json& definition) {
    if (parent.empty()) return badArg("parent");
    if (!definition.is_object()) return badArg("definition");
    auto dir = solutions::saveSolution(parent, folder, definition);
    if (!dir) return dir.error();
    refreshLibrary();
    return *dir;
}

Result<std::string> Session::applySolution(const std::string& packageId, const Json& inputs, const Json& choices, const std::string& cut) {
    auto ids = applySolutionAxes(packageId, inputs, {}, choices, cut);
    if (!ids) return ids.error();
    return ids->front();
}

Result<std::vector<std::string>> Session::applySolutionAxes(const std::string& packageId, const Json& inputs, const std::vector<std::string>& axes,
                                                            const Json& choices, const std::string& cut) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    if (!inputs.is_object()) return badArg("inputs");
    if (!choices.is_object()) return badArg("choices");
    if (!cut.empty() && cut != "assembly" && cut != "parts") return badArg("cut");
    syncAssemblyPositions();
    const solutions::CatalogItem* item = solutionCatalog()->find(packageId);
    if (!item) return notFound(packageId);
    // Several axes (core-api §4.1и, solutions.md §7.11; FX10-WZ-005): one connection with a bolt on each axis — the frame
    // axis input of the solution is the list of the axes, the other inputs and the choices are shared; an axis needs the
    // solution's frame axis. The plans (and the library tables they read) — per axis.
    const std::string axisInput = solutions::parseDefinition(item->solution.definition).frameAxis;
    if (!axes.empty() && axisInput.empty()) return badArg("axes");
    std::vector<Json> perConnection;  // plans per axis
    if (axes.empty()) perConnection.push_back(inputs);
    for (const auto& a : axes) {
        Json in = inputs;
        in[axisInput] = a;
        perConnection.push_back(std::move(in));
    }
    doc::Document nd = doc();
    const doc::SolutionCopy copy = solutions::embed(item->solution);
    if (!nd.solution(copy.id)) nd.solutions.push_back(copy);
    // Library copies of the library tables the plans read — only the used packages, not all of the solution's
    // (solutions.md §5.2); the plan over the final state reuses the wizard's pack measurement.
    Json tables = Json::object();
    const auto cat = libraryCatalog();
    {
        auto used = std::make_shared<std::set<std::string>>();
        const auto provider = tracking(catalogTables(item->solution, cat), used);
        for (const Json& in : perConnection) (void)planOf(item->solution.definition, provider, {}, StateRef::final(), in, choices, false);
        embedUsedTables(nd, copy.definition, *used, tables, *cat);
    }
    // Points of the bolts (solutions.md §7.12, FX8-BLT-002): a list of points for the point input becomes an assembly
    // sketch on the head face with these points; the connections refer to it (a point added to the sketch — a bolt more).
    const solutions::Definition D = solutions::parseDefinition(item->solution.definition);
    const bool byAxis = !axes.empty() || (!D.frameAxis.empty() && inputs.contains(D.frameAxis));
    if (!D.framePoint.empty() && inputs.contains(D.framePoint) && isPointList(inputs[D.framePoint]) && byAxis) {
        for (auto& in : perConnection) in[D.framePoint] = inputs[D.framePoint][0];  // the axis places the frame (§7.3)
    } else if (!D.framePoint.empty() && inputs.contains(D.framePoint) && isPointList(inputs[D.framePoint])) {
        if (!inputs.contains(D.frameFace) || !inputs[D.frameFace].is_string()) return badArg("inputs");
        auto sk = pointsSketch(nd, inputs[D.frameFace].get<std::string>(), inputs[D.framePoint]);
        if (!sk) return sk.error();
        for (auto& in : perConnection) in[D.framePoint] = Json{{"sketch", *sk}};
    }
    // One feature: a single axis — its reference; several — the list (§7.11).
    if (axes.size() > 1) {
        Json in = perConnection.front();
        in[axisInput] = axes;
        perConnection = {std::move(in)};
    }
    std::vector<std::string> ids;
    for (const Json& in : perConnection) {
        const std::string id = doc::allocate(nd, doc::IdKind::Feature);
        Json fj = Json::object();
        fj["id"] = id;
        fj["type"] = "connection";
        fj["name"] = defaultConnectionName(nd.active(), libraryText(copy.title));  // library.md §17.4
        fj["suppressed"] = false;
        fj["solution"] = copy.id;
        fj["inputs"] = in;
        fj["choices"] = choices;
        if (!tables.empty()) fj["tables"] = tables;
        if (cut == "parts") fj["cut"] = "parts";
        auto r = applyFeatureJson(nd, id, fj, insertionIndex(nd.active()), false);
        if (!r) return r.error();
        nd = std::move(*r);
        ids.push_back(id);
    }
    materializeConnections(nd, ids);  // solutions.md §7.10 item 5а
    if (auto s = commit(std::move(nd), "undo.applySolution", {}, ids.front()); !s) return s.error();
    return ids;
}

Status Session::updateSolution(const std::string& featureId, const Json& inputs, const Json& choices, const std::optional<std::string>& cut) {
    if (auto s = checkNoDrag(); !s) return s;
    auto r = updatedSolution(featureId, inputs, choices, cut);
    if (!r) return r.error();
    materializeConnections(*r, {featureId});  // solutions.md §7.10 item 5а
    return commit(std::move(*r), "undo.updateSolution", {}, featureId);
}

// The document with the connection changed (updateSolution), not materialized and not committed.
Result<doc::Document> Session::updatedSolution(const std::string& featureId, const Json& inputs, const Json& choices,
                                               const std::optional<std::string>& cut) {
    const doc::Element& el = doc().active();
    const int i = el.indexOf(featureId);
    if (i < 0 || el.features[size_t(i)]->type != doc::FeatureType::Connection) return notFound(featureId);
    if (!inputs.is_object()) return badArg("inputs");
    if (!choices.is_object()) return badArg("choices");
    if (cut && !cut->empty() && *cut != "assembly" && *cut != "parts") return badArg("cut");
    syncAssemblyPositions();
    Json fj = doc::featureToJsonValue(*el.features[size_t(i)]);
    fj["inputs"] = inputs;
    fj["choices"] = choices;
    doc::Document base = doc();
    // Library packages the changed choices need and the document lacks are embedded (solutions.md §5.2).
    if (const doc::SolutionCopy* copy = base.solution(fj["solution"].get<std::string>())) {
        Json tableMap = fj.contains("tables") ? fj["tables"] : Json::object();
        auto used = std::make_shared<std::set<std::string>>();
        const auto provider = tracking(withCatalogFallback(regen::connectionTables(base, *copy, tableMap), copy->definition, libraryCatalog()), used);
        const int at = el.indexOf(featureId);
        (void)planOf(copy->definition, provider, {}, at == 0 ? StateRef::initial() : StateRef::after(el.features[size_t(at - 1)]->id), inputs,
                     choices, false);
        const doc::Json definition = copy->definition;
        embedUsedTables(base, definition, *used, tableMap, *libraryCatalog());
        if (!tableMap.empty()) fj["tables"] = tableMap;
    }
    if (cut) {
        if (*cut == "parts") fj["cut"] = "parts";
        else fj.erase("cut");
    }
    return applyFeatureJson(base, featureId, fj, i, true);
}

Result<std::vector<std::string>> Session::updateSolutionAxes(const std::string& featureId, const Json& inputs,
                                                             const std::vector<std::string>& axes, const Json& choices,
                                                             const std::optional<std::string>& cut) {
    // The connection keeps all the axes (solutions.md §7.11, FX10-WZ-005): one axis — its reference, several — the list.
    Json in = inputs;
    if (!axes.empty() && doc().active().indexOf(featureId) >= 0)
        if (const auto f = doc().active().find(featureId); f && f->pattern())
            if (const doc::SolutionCopy* c = doc().solution(f->pattern()->def["solution"].get<std::string>())) {
                const std::string ai = solutions::parseDefinition(c->definition).frameAxis;
                if (ai.empty()) return badArg("axes");
                if (in.is_object()) in[ai] = axes.size() == 1 ? Json(axes.front()) : Json(axes);
            }
    if (auto s = updateSolution(featureId, in, choices, cut); !s) return s.error();
    return std::vector<std::string>{featureId};
}

// An assembly sketch on the face faceRef with the points (world coordinates) as standalone points (solutions.md §7.12):
// the frame of a sketch on a face (sketch.md §6) over the final state; inserted at the insertion index of nd.
Result<Plane> Session::faceSketchFrame(const std::string& faceRef) const {
    auto st = stateOf(StateRef::final());
    if (!st || !*st) return apiError("API_STATE_UNAVAILABLE");
    const auto rr = naming::resolve(faceRef, naming::EntityKind::Face, (*st)->names);
    if (rr.error || rr.entities.empty()) return Error("API_INVALID_ARGUMENT").with("name", std::string("face"));
    const auto& fe = (*st)->names.faces.at(size_t(rr.entities.front().index));
    const kernel::Shape& sh = (*st)->bodies.at(size_t(fe.body)).shape;
    if (k_.faceSurface(sh, fe.face).kind != kernel::SurfaceInfo::Kind::Plane) return Error("API_INVALID_ARGUMENT").with("name", std::string("face"));
    const Point3 c = k_.faceCentroid(sh, fe.face);
    return sketch::faceFrame(c, k_.faceNormalAt(sh, fe.face, c));
}

Result<std::string> Session::placeFacePoint(const std::optional<std::string>& sketchId, const std::string& faceRef,
                                            const std::array<double, 3>& at) {
    if (auto s = checkNoDrag(); !s) return s.error();
    const Json pt = Json::array({at[0], at[1], at[2]});
    if (!sketchId) {
        doc::Document nd = doc();
        auto sid = pointsSketch(nd, faceRef, Json::array({pt}));
        if (!sid) return sid.error();
        if (auto s = commit(std::move(nd), "undo.createSketch", {}, *sid); !s) return s.error();
        return sid;
    }
    const doc::Element& el = doc().active();
    const int i = el.indexOf(*sketchId);
    if (i < 0 || !el.features[size_t(i)]->sketchData()) return notFound(*sketchId);
    // The frame of the sketch: its computed state, otherwise (not computed yet) its face (sketch.md §6).
    const std::string face = el.features[size_t(i)]->sketchData()->plane.face;
    Result<Plane> frame = Error("API_STATE_UNAVAILABLE");
    if (auto ss = sketchState(*sketchId)) frame = ss->frame;
    else if (!face.empty()) frame = faceSketchFrame(face);
    if (!frame) return frame.error();
    doc::Document nd = doc();
    Json fj = doc::featureToJsonValue(*el.features[size_t(i)]);
    const Point2 q = frame->toLocal(Point3(at[0], at[1], at[2]));
    fj["entities"].push_back(Json{{"id", doc::allocate(nd, doc::IdKind::Entity)}, {"type", "point"}, {"x", q.x()}, {"y", q.y()}});
    auto r = applyFeatureJson(nd, *sketchId, fj, i, true);
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.sketchAdd", {*sketchId}, *sketchId); !s) return s.error();
    return *sketchId;
}

// Points of a sketch of the point input by the document (solutions.md §7.12): the stored coordinates of its standalone
// points on the frame of its face — for a sketch the command has just created (its state is not computed yet).
Result<std::optional<std::vector<Point3>>> Session::documentSketchPoints(const doc::Element& el, const solutions::Definition& def,
                                                                         const Json& inputs) const {
    if (def.framePoint.empty() || !inputs.contains(def.framePoint) || !inputs[def.framePoint].is_object()) return std::optional<std::vector<Point3>>();
    const std::string id = inputs[def.framePoint].value("sketch", std::string());
    const auto f = el.find(id);
    if (!f || !f->sketchData() || f->sketchData()->plane.face.empty()) return Error("REGEN_INPUT_ERROR").with("id", id);
    auto frame = faceSketchFrame(f->sketchData()->plane.face);
    if (!frame) return frame.error();
    std::vector<Point3> out;
    for (const auto& e : f->sketchData()->entities)
        if (e.type == sketch::EntityType::Point) out.push_back(frame->toWorld(Point2(e.x, e.y)));
    if (out.empty()) return Error("SOL_NO_POINTS").with("name", def.framePoint).with("id", id);
    return std::optional(std::move(out));
}

Result<std::string> Session::pointsSketch(doc::Document& nd, const std::string& faceRef, const Json& points) const {
    auto fr = faceSketchFrame(faceRef);
    if (!fr) return fr.error();
    const Plane frame = *fr;
    const std::string sid = doc::allocate(nd, doc::IdKind::Feature);
    Json sj = Json::object();
    sj["id"] = sid;
    sj["type"] = "sketch";
    sj["name"] = doc::defaultFeatureName(nd.active(), doc::FeatureType::Sketch, std::string());
    sj["suppressed"] = false;
    sj["plane"] = Json{{"face", faceRef}};
    sj["entities"] = Json::array();
    for (const auto& p : points) {
        const Point2 q = frame.toLocal(Point3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
        sj["entities"].push_back(Json{{"id", doc::allocate(nd, doc::IdKind::Entity)}, {"type", "point"}, {"x", q.x()}, {"y", q.y()}});
    }
    sj["constraints"] = Json::array();
    auto r = applyFeatureJson(nd, sid, sj, insertionIndex(nd.active()), false);
    if (!r) return r.error();
    nd = std::move(*r);
    return sid;
}

Status Session::setConnectionEditing(const std::optional<std::string>& featureId) {
    if (featureId) {
        const doc::FeaturePtr f = doc().active().find(*featureId);
        if (!doc().active().isAssembly() || !f || f->type != doc::FeatureType::Connection) return notFound(*featureId);
    }
    if (connectionEditing_ == featureId) return okStatus();
    connectionEditing_ = featureId;
    scheduleRegen();
    return okStatus();
}

doc::Document Session::regenDocument() const {
    if (!connectionEditing_) return doc();
    const doc::Element& A = doc().active();
    const int i = A.indexOf(*connectionEditing_);
    if (i < 0) return doc();
    // The state on the input of the connection (solutions.md §7.11): the assembly rolled back to just before it — its
    // fasteners and cuts (and those of later assembly features) are not evaluated, the faces are named as in its
    // input state V. An earlier rollback marker stays.
    const int rollbackAt = A.rollback.kind == doc::Rollback::Kind::Start ? 0
                           : A.rollback.kind == doc::Rollback::Kind::After ? A.indexOf(A.rollback.feature) + 1
                                                                          : int(A.features.size());
    if (rollbackAt <= i) return doc();
    // The owned cuts of the parts are not touched (FX8-BLT-001): in the owner assembly they are excluded anyway
    // (§7.10 item 3.1), so the parts are not rebuilt on entering and leaving the edit.
    doc::Document nd = doc();
    doc::Element& E = nd.active();
    E.rollback = i == 0 ? doc::Rollback{doc::Rollback::Kind::Start, {}} : doc::Rollback{doc::Rollback::Kind::After, E.features[size_t(i - 1)]->id};
    return nd;
}

std::optional<Json> Session::connectionReport(const std::string& featureId) const {
    auto st = stateOf(StateRef::final());
    if (!st || !*st) return std::nullopt;
    auto it = (*st)->connections.find(featureId);
    if (it == (*st)->connections.end()) return std::nullopt;
    std::optional<Json> out;
    out.emplace(it->second);
    return out;
}

// ---------------------------------------------------------------- owned cuts (solutions.md §7.10)

namespace {

bool isPartsMode(const doc::Feature& f) {
    const doc::Json& d = f.pattern()->def;
    return d.contains("cut") && d["cut"] == "parts";
}

// Owned connection cuts of assembly el by the published result (§7.10 item 4): the state of every connection.
std::vector<std::pair<std::string, OwnedConnection>> publishedConnections(const doc::Element& el, const regen::PartResult& r) {
    std::vector<std::pair<std::string, OwnedConnection>> out;
    for (const auto& f : el.features) {
        // Connections and assembly holes (an assembly hole "in the parts" owns features too, assembly.md §7г.6).
        if (f->type != doc::FeatureType::Connection && f->type != doc::FeatureType::Hole) continue;
        OwnedConnection oc;
        oc.name = f->name;
        auto st = r.featureStatus.find(f->id);
        const doc::Json* report = nullptr;
        if (r.final)
            if (auto it = r.final->connections.find(f->id); it != r.final->connections.end()) report = &it->second;
        if (!isPartsMode(*f)) oc.kind = OwnedConnection::Kind::Remove;
        else if (st != r.featureStatus.end() &&
                 (st->second.status == regen::FeatureStatus::Suppressed || st->second.status == regen::FeatureStatus::RolledBack))
            oc.kind = OwnedConnection::Kind::Suppress;
        else if (report) {
            oc.kind = OwnedConnection::Kind::Report;
            oc.report = *report;
        } else oc.kind = OwnedConnection::Kind::Keep;
        out.emplace_back(f->id, std::move(oc));
    }
    return out;
}

}  // namespace

void Session::materializeConnections(doc::Document& nd, const std::vector<std::string>& connections) const {
    const doc::Element& A = nd.active();
    if (!A.isAssembly()) return;
    OwnedScope scope;
    scope.assembly = A.id;
    scope.title = [this](const std::string& t) { return libraryText(t); };  // library.md §17.4
    scope.all = false;
    const std::string self = selfPath();
    const std::string aid = A.id;
    scope.owned = [aid, self](const doc::Feature& f) { return regen::ownedByAssembly(f, aid, true, self, self); };
    const int rollbackAt = A.rollback.kind == doc::Rollback::Kind::Start ? 0
                           : A.rollback.kind == doc::Rollback::Kind::After ? A.indexOf(A.rollback.feature) + 1
                                                                          : int(A.features.size());
    for (const auto& cid : connections) {
        OwnedConnection oc;
        const int i = A.indexOf(cid);
        const doc::FeaturePtr f = i >= 0 ? A.features[size_t(i)] : nullptr;
        const bool owning = f && (f->type == doc::FeatureType::Connection || f->type == doc::FeatureType::Hole);
        if (!owning || !isPartsMode(*f)) {
            scope.connections.emplace_back(cid, oc);  // Remove
            continue;
        }
        oc.name = f->name;
        if (f->suppressed || i >= rollbackAt) {
            oc.kind = OwnedConnection::Kind::Suppress;
            scope.connections.emplace_back(cid, oc);
            continue;
        }
        if (f->type == doc::FeatureType::Hole) {  // its owned features follow the published result (assembly.md §7г.6)
            oc.kind = OwnedConnection::Kind::Keep;
            scope.connections.emplace_back(cid, oc);
            continue;
        }
        // The plan over the state before the feature (as solutionPlan, §8); unavailable or failed — kept as is.
        oc.kind = OwnedConnection::Kind::Keep;
        const doc::Json& d = f->pattern()->def;
        const doc::SolutionCopy* copy = nd.solution(d["solution"].get<std::string>());
        // The state before the feature; sketches just before it (the points sketch made by this command, §7.12) do not
        // change the bodies — without a state of their own the state before them is taken.
        int before = i - 1;
        auto st = stateOf(before < 0 ? StateRef::initial() : StateRef::after(A.features[size_t(before)]->id));
        while (!st && before >= 0 && A.features[size_t(before)]->sketchData()) {
            --before;
            st = stateOf(before < 0 ? StateRef::initial() : StateRef::after(A.features[size_t(before)]->id));
        }
        if (copy && st && *st && solutions::checkDefinition(copy->definition)) {
            const doc::Json tableMap = d.contains("tables") ? d["tables"] : doc::Json::object();
            std::map<std::string, std::string> libraries;
            for (const auto& [k, v] : tableMap.items()) libraries[k] = v.get<std::string>();
            // The pack measured by the wizard over the same state is reused (FX8-BLT-001).
            std::lock_guard<std::mutex> lock(packMu_);
            if (packState_ != *st) {
                packState_ = *st;
                packCache_ = regen::PackCache{};
            }
            packCache_.measure = true;
            const solutions::Definition def = solutions::parseDefinition(copy->definition);
            const auto tables = regen::connectionTables(nd, *copy, tableMap);
            // Several points of a sketch (§7.12) or axes (§7.11): a plan per position, the reports merged as by the
            // evaluator.
            std::vector<Json> perPoint;
            bool byAxes = false;
            if (auto pos = regen::connectionPositions(A, **st, def, d["inputs"], &byAxes)) {
                perPoint = std::move(*pos);
            } else if (auto pts = documentSketchPoints(A, def, d["inputs"]); pts) {  // a sketch made by this command: not in
                                                                                    // the state yet
                if (*pts)
                    for (const auto& p : **pts) perPoint.push_back(regen::inputsAtPoint(def, d["inputs"], p));
                else
                    perPoint.push_back(d["inputs"]);
            }
            std::vector<Json> reports;
            for (const auto& in : perPoint) {
                const regen::ConnectionPlan plan = regen::planConnection(k_, def, tables, in, d["choices"], **st, nullptr, &packCache_);
                if (plan.plan.error) break;
                reports.push_back(regen::connectionReport(plan, libraries, {}, st->get()));
            }
            if (!perPoint.empty() && reports.size() == perPoint.size()) {
                oc.kind = OwnedConnection::Kind::Report;
                oc.report = regen::mergePointReports(reports, byAxes ? "axes" : "points");
            }
        }
        scope.connections.emplace_back(cid, std::move(oc));
    }
    syncOwnedCuts(nd, scope);
}

void Session::syncOwnedCutsFromPublished() {
    if (!doc().active().isAssembly() || connectionEditing_) return;  // the published state hides the edited connection
    PartResultPtr r;
    {
        std::lock_guard lk(mu_);
        if (!published_ || published_->revision != revision_ || !published_->final) return;
        r = published_;
    }
    const doc::Element& A = doc().active();
    OwnedScope scope;
    scope.assembly = A.id;
    scope.title = [this](const std::string& t) { return libraryText(t); };  // library.md §17.4
    const std::string self = selfPath();
    const std::string aid = A.id;
    scope.owned = [aid, self](const doc::Feature& f) { return regen::ownedByAssembly(f, aid, true, self, self); };
    scope.connections = publishedConnections(A, *r);
    doc::Document nd = doc();
    if (!syncOwnedCuts(nd, scope)) return;
    // A change of the current snapshot without a new undo step (§7.10 item 5б).
    undo_->replaceCurrent(doc::Snapshot{std::move(nd), undo_->current().label, ctx_, undo_->current().labelName});
    ++revision_;
    if (cb_.onDocumentChanged) cb_.onDocumentChanged(revision_);
    scheduleRegen();
}

Status Session::writeLinkedOwnedCuts(const std::string& newPath, const std::optional<std::string>& oldPath) {
    namespace fs = std::filesystem;
    if (!doc().active().isAssembly()) return okStatus();
    PartResultPtr r;
    {
        std::lock_guard lk(mu_);
        if (!published_ || !published_->final) return okStatus();
        r = published_;
    }
    const std::string asmNew = absoluteU8(newPath), asmOld = oldPath ? absoluteU8(*oldPath) : std::string();
    const doc::Element& A = doc().active();
    const auto conns = publishedConnections(A, *r);
    // Files: desired entries of other files and linked documents with owned cuts of this assembly (§7.10 item 6).
    std::set<std::string> files;
    for (const auto& [id, oc] : conns)
        if (oc.kind == OwnedConnection::Kind::Report)
            for (const auto& cut : oc.report["cuts"])
                if (cut.contains("owned"))
                    for (const auto& e : cut["owned"])
                        if (!e["file"].get<std::string>().empty()) files.insert(e["file"].get<std::string>());
    const std::string aid = A.id;
    auto ownedBy = [aid, asmNew, asmOld](const std::string& file) {
        return [aid, asmNew, asmOld, file](const doc::Feature& f) {
            return regen::ownedByAssembly(f, aid, false, file, asmNew) || (!asmOld.empty() && regen::ownedByAssembly(f, aid, false, file, asmOld));
        };
    };
    for (const auto& [file, ld] : links_) {
        if (!ld.doc || doc::isLibraryRef(file)) continue;
        const auto owned = ownedBy(file);
        for (const auto& el : static_cast<const doc::Document*>(ld.doc)->elements)
            for (const auto& f : el.features)
                if (owned(*f)) files.insert(file);
    }
    std::optional<Error> failure;
    for (const auto& file : files) {
        auto it = links_.find(file);
        if (it == links_.end() || !it->second.doc) continue;
        doc::Document pd = *static_cast<const doc::Document*>(it->second.doc);
        OwnedScope scope;
        scope.assembly = aid;
        scope.title = [this](const std::string& t) { return libraryText(t); };  // library.md §17.4
        scope.ownerFile = relativeU8(asmNew, regen::parentDir(file));
        scope.partKey = file;
        scope.owned = ownedBy(file);
        scope.connections = conns;
        if (!syncOwnedCuts(pd, scope)) continue;
        if (auto s = io::writeFileAtomic(file, doc::serialize(pd)); !s) {
            if (!failure) failure = s.error();
            continue;
        }
        // The read copy is replaced with the written one: no "update by links" notice (§7.10 item 6).
        auto keep = std::make_shared<const doc::Document>(std::move(pd));
        it->second.doc = keep.get();
        it->second.keep = keep;
        it->second.error.reset();
        linkStats_[file] = statOf(file);
    }
    if (failure) return *failure;
    return okStatus();
}

Status Session::checkOwnedFeature(const std::string& id, bool deleting) const {
    const doc::FeaturePtr f = doc().active().find(id);
    if (!f || f->type != doc::FeatureType::ConnectionCut || !f->pattern()) return okStatus();
    const doc::Json& o = f->pattern()->def["owner"];
    if (deleting && !o.contains("file")) {  // an orphan of this document may be deleted (§7.10 item 2)
        const int ai = doc().elementIndex(o["assembly"].get<std::string>());
        if (o.contains("library")) {  // a cut of a library element: an orphan without its instance (library.md §6.6)
            const auto* el = ai >= 0 ? &doc().elements[size_t(ai)] : nullptr;
            if (!el || std::none_of(el->instances.begin(), el->instances.end(), [&](const doc::Instance& x) { return x.id == o["connection"].get<std::string>(); }))
                return okStatus();
        } else {
            const doc::FeaturePtr c = ai >= 0 ? doc().elements[size_t(ai)].find(o["connection"].get<std::string>()) : nullptr;
            if (!c || (c->type != doc::FeatureType::Connection && c->type != doc::FeatureType::Hole)) return okStatus();
        }
    }
    if (o.contains("library")) return Error("API_OWNED_LIBRARY_CUT").with("id", id).with("name", o["name"].get<std::string>());
    return Error("API_OWNED_FEATURE").with("id", id).with("name", o["name"].get<std::string>());
}

}  // namespace construe::api
