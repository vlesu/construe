// STEP: export of an object with structure, names and colors; import of bodies and assemblies
// (spec/core-api.md §4.7б, kernel-interface.md §3.16; S2-IO-004, S2-DOC-001, S2-PART-004).
#include "api/session.h"

#include "api/errors.h"
#include "rec/recognize.h"
#include "regen/naming.h"
#include "regen/regen.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace construe::api {

namespace {

std::optional<std::string> partColorOf(const doc::Element& el) {
    if (el.color) return *el.color;
    if (el.material && el.material->contains("color")) return (*el.material)["color"].get<std::string>();
    return std::nullopt;
}

kernel::Motion kmotion(const regen::Motion3& m) {
    kernel::Motion k;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) k.r[size_t(3 * i + j)] = m.R(i, j);
    k.t = m.t;
    return k;
}

regen::Motion3 inverse(const regen::Motion3& m) { return {m.R.transpose(), -(m.R.transpose() * m.t)}; }

// a∘b: b first, then a.
kernel::Motion compose(const kernel::Motion& a, const kernel::Motion& b) {
    Eigen::Matrix3d ra, rb;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) ra(i, j) = a.r[size_t(3 * i + j)], rb(i, j) = b.r[size_t(3 * i + j)];
    const Eigen::Matrix3d r = ra * rb;
    kernel::Motion out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out.r[size_t(3 * i + j)] = r(i, j);
    out.t = ra * b.t + a.t;
    return out;
}

// Part product from a state (core-api §4.7б): shape - combine of bodies, face colors - from the combine history.
Result<kernel::StepProduct> partProduct(const kernel::Kernel& k, const regen::PartState& st, const doc::Element* el, const std::string& name) {
    if (st.bodies.empty()) return Error("EXPORT_NO_BODIES");
    std::vector<kernel::Shape> shapes;
    for (const auto& b : st.bodies) shapes.push_back(b.shape);
    auto comb = k.combine(shapes);
    kernel::StepProduct p;
    p.name = name;
    p.shape = comb.shape;
    if (el) {
        p.color = partColorOf(*el);
        // (body, body face) → shape face.
        std::map<std::pair<int, int>, int> faceOf;
        for (size_t r = 0; r < comb.history.faceOrigins.size(); ++r)
            for (const auto& o : comb.history.faceOrigins[r])
                if (o.kind == kernel::Origin::Kind::FromFace) faceOf[{o.input.operand, o.input.face}] = int(r);
        for (const auto& [face, color] : el->faceColors) {
            const auto rr = naming::resolve(face, naming::EntityKind::Face, st.names);
            if (naming::refFate(rr, true) != naming::RefFate::Resolved) continue;
            for (const auto& e : rr.entities) {
                const auto& fe = st.names.faces.at(e.index);
                if (auto it = faceOf.find({fe.body, fe.face}); it != faceOf.end()) p.faceColors[it->second] = color;
            }
        }
    }
    return p;
}

// STEP model of an object (core-api §4.7б) from the result of its regeneration (active element d).
Result<kernel::StepModel> stepModelOf(const kernel::Kernel& k, const doc::Document& d, const regen::PartResult& r) {
    kernel::StepModel m;
    const doc::Element& el = d.active();
    if (!el.isAssembly()) {
        if (!r.final) return Error("API_STATE_UNAVAILABLE");
        auto p = partProduct(k, *r.final, &el, el.name);
        if (!p) return p.error();
        m.products.push_back(std::move(*p));
        m.root = 0;
        return m;
    }
    if (!r.assembly) return Error("API_STATE_UNAVAILABLE");
    const auto& inst = r.assembly->instances;
    std::map<std::string, int> productOf;  // file#element → product
    auto elementOf = [&](const regen::InstanceEntry& e) -> const doc::Element* {
        if (!e.file.empty()) return nullptr;
        const int ei = d.elementIndex(e.element);
        return ei >= 0 ? &d.elements[size_t(ei)] : nullptr;
    };
    // Product of an instance (recursively over children - records with path "<path>/…" at a greater depth).
    std::function<Result<int>(size_t)> productFor = [&](size_t idx) -> Result<int> {
        const auto& e = inst[idx];
        const std::string key = e.file + "#" + e.element;
        if (auto it = productOf.find(key); it != productOf.end()) return it->second;
        const doc::Element* pe = elementOf(e);
        std::string base = e.name;  // object name of another file - the instance name without " <N>"
        if (const auto lt = base.rfind(" <"); lt != std::string::npos && !base.empty() && base.back() == '>') base = base.substr(0, lt);
        const std::string pname = pe ? pe->name : base;
        kernel::StepProduct p;
        if (e.kind == "assembly") {
            p.name = pname;
            for (size_t j = idx + 1; j < inst.size(); ++j) {
                const auto& c = inst[j];
                if (c.depth != e.depth + 1 || c.path.rfind(e.path + "/", 0) != 0) continue;
                if (!c.state && c.kind != "assembly") continue;
                if (c.suppressed || c.error) continue;
                auto cp = productFor(j);
                if (!cp) return cp.error();
                p.components.push_back({*cp, kmotion(inverse(e.world) * c.world), c.name});
            }
        } else {
            auto pp = partProduct(k, *e.state, pe, pname);
            if (!pp) return pp.error();
            p = std::move(*pp);
        }
        const int id = int(m.products.size());
        m.products.push_back(std::move(p));
        productOf[key] = id;
        return id;
    };
    kernel::StepProduct root;
    root.name = el.name;
    for (size_t i = 0; i < inst.size(); ++i) {
        const auto& e = inst[i];
        if (e.depth != 0 || e.suppressed || e.error || (!e.state && e.kind != "assembly")) continue;
        auto cp = productFor(i);
        if (!cp) return cp.error();
        root.components.push_back({*cp, kmotion(e.world), e.name});
    }
    m.root = int(m.products.size());
    m.products.push_back(std::move(root));
    return m;
}

std::string fileBaseName(const std::string& path) {
    const auto u8 = std::filesystem::u8path(path).filename().u8string();
    return std::string(u8.begin(), u8.end());
}

// File name without the path and the last extension (FX10-IO-002).
std::string fileStem(const std::string& name) {
    const auto u8 = std::filesystem::u8path(name).stem().u8string();
    std::string s(u8.begin(), u8.end());
    return s.empty() ? name : s;
}

// The first free name among the document's elements: name, "name 2", "name 3", … (core-api §4.7б, FX10-IO-002).
std::string uniqueElementName(const doc::Document& d, const std::string& name) {
    auto taken = [&](const std::string& n) {
        for (const auto& e : d.elements)
            if (e.name == n) return true;
        return false;
    };
    std::string out = name;
    for (int n = 2; taken(out); ++n) out = name + " " + std::to_string(n);
    return out;
}

}  // namespace

Result<ExportId> Session::exportStep(const std::string& p, const std::optional<std::string>& element) {
    if (element && element->empty()) return notFound(*element);
    auto d = exportDocument(element.value_or(std::string()));
    if (!d) return d.error();
    return enqueueExport(std::move(*d), [this, p](const doc::Document& snap, const regen::PartResult& pr, const regen::LinkMap&) -> Status {
        auto m = stepModelOf(k_, snap, pr);
        if (!m) return m.error();
        return k_.writeStep(p, *m);
    });
}

Result<std::vector<std::string>> Session::importStep(const std::string& path, const StepImportOptions& options, StepImportReport* report) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    auto p = prepareImportStep(path, options);
    if (!p) return p.error();
    return applyImportStep(std::move(*p), report);
}

// Preparation (§4.7з): reading, healing and B-Rep of every part product — the long part; the document is not touched.
Result<StepImportPrepared> Session::prepareImportStep(const std::string& path, const StepImportOptions& options, const Cancel* cancel) const {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    StepImportPrepared out;
    out.revision = revision_;
    out.source = fileBaseName(path);
    out.dedupe = options.dedupe;
    if (cancelled(cancel)) return Error("API_CANCELLED");
    auto m = k_.readStep(path);
    if (!m) return m.error();
    out.model = std::move(*m);
    out.nameByFile = options.nameByFile;
    // One part (FX10-IO-001): a root assembly whose chain of single occurrences ends at a part gives only that part,
    // its shape moved by the composed occurrence positions.
    if (options.singlePart) {
        int cur = out.model.root;
        kernel::Motion acc;
        while (cur >= 0 && cur < int(out.model.products.size()) && !out.model.products[size_t(cur)].shape &&
               out.model.products[size_t(cur)].components.size() == 1) {
            const auto& c = out.model.products[size_t(cur)].components.front();
            acc = compose(acc, c.motion);
            cur = c.product;
        }
        if (cur != out.model.root && cur >= 0 && cur < int(out.model.products.size()) && out.model.products[size_t(cur)].shape) {
            auto& p = out.model.products[size_t(cur)];
            if (auto moved = k_.transform(*p.shape, acc)) p.shape = moved->shape;
            out.single = cur;
        }
    }
    const int n = int(out.model.products.size());
    out.bodies.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        reportProgress(cancel, i, n);
        if (cancelled(cancel)) return Error("API_CANCELLED");
        const kernel::StepProduct& p = out.model.products[size_t(i)];
        if (!p.shape || (out.single >= 0 && i != out.single)) continue;
        // Healing a shape with entity tolerance > KERNEL_MAX_ENTITY_TOL (§4.7б, kernel-interface §3.24; S4-REC-013)
        kernel::Shape shape = *p.shape;
        StepImportPrepared::Body body;
        if (auto h = k_.healShape(shape, options.healTolerance); h && h->toleranceBefore > kernel::KERNEL_MAX_ENTITY_TOL) {
            body.heal = StepImportReport::Body{{}, h->toleranceBefore, h->healed, h->toleranceAfter};
            if (h->healed) shape = h->shape;
        }
        auto brep = k_.shapeToBrep(shape);
        if (!brep) continue;
        body.brep = std::move(*brep);
        out.bodies[size_t(i)] = std::move(body);
    }
    reportProgress(cancel, n, n);
    if (cancelled(cancel)) return Error("API_CANCELLED");
    return out;
}

Result<std::vector<std::string>> Session::applyImportStep(StepImportPrepared prepared, StepImportReport* report) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    if (prepared.revision != revision_) return Error("API_STALE");
    const kernel::StepModel* m = &prepared.model;
    syncAssemblyPositions();
    doc::Document nd = doc();
    const std::string& source = prepared.source;
    std::vector<std::string> elemOf(m->products.size());
    std::vector<std::string> created;
    std::vector<StepImportReport::Body> healed;
    // Deduplication (§4.7б, reconstruction.md §5.7; FX7-IO-001): a part product identical to an earlier one (same color
    // and face color set) is replaced by it — alias[i] = (representative, motion of the representative onto product i).
    std::map<int, std::pair<int, kernel::Motion>> alias;
    if (prepared.dedupe) {
        std::map<std::string, std::vector<int>> buckets;  // colors → part products in order
        for (size_t i = 0; i < m->products.size(); ++i) {
            const auto& p = m->products[i];
            if (!p.shape || int(i) == m->root) continue;
            std::vector<std::string> fc;
            for (const auto& [fi, c] : p.faceColors) fc.push_back(c);
            std::sort(fc.begin(), fc.end());
            std::string key = p.color.value_or("-");
            for (const auto& c : fc) key += "," + c;
            buckets[key].push_back(int(i));
        }
        for (const auto& [key, idx] : buckets) {
            if (idx.size() < 2) continue;
            std::vector<kernel::Shape> shapes;
            for (int i : idx) shapes.push_back(*m->products[size_t(i)].shape);
            const auto groups = rec::groupCongruent(k_, shapes);
            for (size_t g = 0; g < idx.size(); ++g)
                if (groups[g].group != int(g)) alias[idx[g]] = {idx[size_t(groups[g].group)], groups[g].motion};
        }
    }
    // Products - in file order; inserted products come before assemblies in the model (kernel-interface §3.16).
    // Object names (§4.7б): by the file (FX10-IO-002) or by the product; unique among the document's elements.
    const std::string stem = fileStem(source);
    const int rootProduct = prepared.single >= 0 ? prepared.single : m->root;
    auto nameOf = [&](int i, const std::string& kind) {
        const std::string& pn = m->products[size_t(i)].name;
        std::string n = pn.empty() ? doc::defaultElementName(nd, kind) : pn;
        if (prepared.nameByFile) n = i == rootProduct ? stem : stem + " - " + n;
        return uniqueElementName(nd, n);
    };
    std::function<void(int)> make = [&](int i) {
        if (!elemOf[size_t(i)].empty() || alias.count(i)) return;
        const kernel::StepProduct& p = m->products[size_t(i)];
        if (p.shape) {
            const auto& prep = prepared.bodies[size_t(i)];
            if (!prep) return;
            std::optional<StepImportReport::Body> heal = prep->heal;
            const std::string* brep = &prep->brep;
            doc::Element e;
            e.id = doc::allocate(nd, doc::IdKind::Element);
            e.kind = "body";
            e.name = nameOf(i, "body");
            e.color = p.color;
            auto f = std::make_shared<doc::Feature>();
            f->id = doc::allocate(nd, doc::IdKind::Feature);
            f->type = doc::FeatureType::Import;
            f->name = doc::defaultFeatureName(e, doc::FeatureType::Import);
            doc::PatternData pd;
            pd.def = doc::Json{{"brep", *brep}, {"source", source}};
            f->data = std::move(pd);
            for (const auto& [fi, color] : p.faceColors) e.faceColors.push_back({f->id + ".face(" + std::to_string(fi + 1) + ")", color});
            std::sort(e.faceColors.begin(), e.faceColors.end());
            e.features.push_back(f);
            e.rollback = {doc::Rollback::Kind::End, {}};
            elemOf[size_t(i)] = e.id;
            created.push_back(e.id);
            if (heal) {
                heal->element = e.id;
                healed.push_back(*heal);
            }
            nd.elements.push_back(std::move(e));
            return;
        }
        if (p.components.empty()) return;
        for (const auto& c : p.components) make(alias.count(c.product) ? alias.at(c.product).first : c.product);
        doc::Element a;
        a.id = doc::allocate(nd, doc::IdKind::Element);
        a.kind = "assembly";
        a.name = nameOf(i, "assembly");
        a.rollback = {doc::Rollback::Kind::End, {}};
        std::set<std::string> taken;
        for (const auto& comp : p.components) {
            kernel::StepComponent c = comp;
            if (auto al = alias.find(c.product); al != alias.end()) {
                // Occurrence of a replaced product: the representative placed by C∘M.
                c.product = al->second.first;
                c.motion = compose(comp.motion, al->second.second);
            }
            const std::string ref = elemOf[size_t(c.product)];
            if (ref.empty()) continue;
            doc::Instance in;
            in.id = doc::allocate(nd, doc::IdKind::Instance);
            in.refElement = ref;
            std::string refName;
            for (const auto& x : nd.elements)
                if (x.id == ref) refName = x.name;
            in.name = c.name;
            for (int n = 1; in.name.empty() || taken.count(in.name); ++n) in.name = refName + " <" + std::to_string(n) + ">";
            taken.insert(in.name);
            Eigen::Matrix3d R;
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col) R(r, col) = c.motion.r[size_t(3 * r + col)];
            const Eigen::Quaterniond q(R);
            in.position = {c.motion.t.x(), c.motion.t.y(), c.motion.t.z()};
            in.rotation = {q.w(), q.x(), q.y(), q.z()};
            in.fixed = a.instances.empty();
            a.instances.push_back(in);
        }
        elemOf[size_t(i)] = a.id;
        created.push_back(a.id);
        nd.elements.push_back(std::move(a));
    };
    if (prepared.single >= 0) make(prepared.single);
    else
        for (size_t i = 0; i < m->products.size(); ++i) make(int(i));
    if (created.empty()) return Error("KERNEL_IMPORT_FAILED").with("detail", std::string("empty"));
    const std::string root = elemOf[size_t(rootProduct)].empty() ? created.back() : elemOf[size_t(rootProduct)];
    nd.activeElement = root;
    if (auto s = commit(std::move(nd), "undo.importStep", {}, root); !s) return s.error();
    if (report) report->bodies = std::move(healed);
    return created;
}

}  // namespace construe::api
