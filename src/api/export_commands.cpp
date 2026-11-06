// STL and 3MF mesh export: choice of object, bodies and instances, file per part, mesh check
// (spec/core-api.md §4.7а, io.md §2а–§2б, §4а; S2-IO-001, S2-IO-002, S2-IO-003, S2-IO-005); flat pattern export to DXF
// (core-api §4.7е, io.md §2д; S5-DXF-001).
#include "api/session.h"

#include "api/errors.h"
#include "regen/naming.h"
#include "regen/regen.h"
#include "printprep/printprep.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace construe::api {

namespace {

// Export piece: a part body in its state, world placement, name, colors, part.
struct Piece {
    std::string name;
    regen::StatePtr state;
    int body = 0;
    regen::Motion3 motion;
    std::optional<std::string> color;
    std::map<std::string, std::string> faceColors;  // full face name → color
    std::string partKey, partFileName;
    std::string path;                     // instance path (assembly)
    const doc::Element* element = nullptr;  // part element (fits for printing); nullptr — not loaded
};

std::optional<std::string> partColor(const doc::Element& el) {
    if (el.color) return *el.color;
    if (el.material && el.material->contains("color")) return (*el.material)["color"].get<std::string>();
    return std::nullopt;
}

// Face color overrides (document.md §2.4) resolved in the part's state: full face name → color.
std::map<std::string, std::string> resolvedFaceColors(const doc::Element& el, const regen::PartState& st) {
    std::map<std::string, std::string> out;
    for (const auto& [face, color] : el.faceColors) {
        const auto rr = naming::resolve(face, naming::EntityKind::Face, st.names);
        if (naming::refFate(rr, true) != naming::RefFate::Resolved) continue;
        for (const auto& e : rr.entities) out[st.names.faces.at(e.index).full] = color;
    }
    return out;
}

std::string sanitizeFileName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out.push_back(c < 0x20 || std::string("\\/:*?\"<>|").find(char(c)) != std::string::npos ? '_' : char(c));
    const auto a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    out = a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
    return out.empty() ? std::string("export") : out;
}

std::string fileNameOf(const doc::Element& el) {
    return sanitizeFileName(el.designation ? *el.designation + " " + el.name : el.name);
}

Status checkOptions(const Session::MeshExportOptions& o) {
    io::StlOptions so;
    so.chordalDeviation = o.chordalDeviation;
    so.angularDeviation = o.angularDeviation;
    if (auto s = io::validateOptions(so); !s) return s;
    if (o.perPart && o.fileFormat != io::MeshFileFormat::Stl)
        return Error("EXPORT_INVALID_OPTIONS").with("name", std::string("perPart")).with("actual", std::string("true"));
    std::set<std::string> seen;
    for (const auto& it : o.items)
        if (!seen.insert(it).second) return Error("EXPORT_INVALID_OPTIONS").with("name", std::string("items")).with("actual", o.items);
    return okStatus();
}

// Element of an instance: of document d or of a loaded linked document (as Session::elementOfEntry).
const doc::Element* entryElement(const doc::Document& d, const regen::LinkMap* links, const regen::InstanceEntry& e) {
    const doc::Document* D = &d;
    if (!e.file.empty()) {
        if (!links) return nullptr;
        auto it = links->find(e.file);
        if (it == links->end() || !it->second.doc) return nullptr;
        D = static_cast<const doc::Document*>(it->second.doc);
    }
    const int i = D->elementIndex(e.element);
    return i < 0 ? nullptr : &D->elements[size_t(i)];
}

// Pieces per the rules of core-api §4.7а item 2; r - regeneration result of the object (active element of document d).
Result<std::vector<Piece>> collectPieces(const doc::Document& d, const regen::PartResult& r, const Session::MeshExportOptions& o,
                                         const regen::LinkMap* links) {
    std::vector<Piece> out;
    const doc::Element& el = d.active();
    const std::set<std::string> items(o.items.begin(), o.items.end());
    if (!el.isAssembly()) {
        const auto& st = r.final;
        if (!st) return Error("API_STATE_UNAVAILABLE");
        for (const auto& it : o.items)
            if (std::none_of(st->bodies.begin(), st->bodies.end(), [&](const regen::Body& b) { return b.id == it; })) return notFound(it);
        const auto fc = resolvedFaceColors(el, *st);
        for (size_t i = 0; i < st->bodies.size(); ++i) {
            if (!items.empty() && !items.count(st->bodies[i].id)) continue;
            Piece p;
            p.name = st->bodies.size() > 1 ? el.name + " " + st->bodies[i].id : el.name;
            p.state = st, p.body = int(i), p.color = partColor(el), p.faceColors = fc;
            p.partKey = el.id, p.partFileName = fileNameOf(el);
            p.element = &el;
            out.push_back(std::move(p));
        }
        return out;
    }
    if (!r.assembly) return Error("API_STATE_UNAVAILABLE");
    for (const auto& it : o.items)
        if (!r.assembly->find(it) || it.find('/') != std::string::npos) return notFound(it);
    std::map<std::string, std::string> names;  // path → instance name
    for (const auto& e : r.assembly->instances) names[e.path] = e.name;
    for (const auto& e : r.assembly->instances) {
        if (!e.state || e.suppressed || e.error || e.kind == "assembly") continue;
        const std::string top = e.path.substr(0, e.path.find('/'));
        if (!items.empty() && !items.count(top)) continue;
        bool off = false;  // suppressed ancestor
        for (size_t p = e.path.find('/'); p != std::string::npos; p = e.path.find('/', p + 1))
            if (const auto* a = r.assembly->find(e.path.substr(0, p)); a && (a->suppressed || a->error)) off = true;
        if (off) continue;
        std::string chain;
        for (size_t p = 0;;) {
            const size_t q = e.path.find('/', p);
            const std::string sub = e.path.substr(0, q);
            chain += (chain.empty() ? "" : " / ") + names[sub];
            if (q == std::string::npos) break;
            p = q + 1;
        }
        const doc::Element* pe = nullptr;
        if (e.file.empty())
            if (const int ei = d.elementIndex(e.element); ei >= 0) pe = &d.elements[size_t(ei)];
        std::map<std::string, std::string> fc;
        if (pe) fc = resolvedFaceColors(*pe, *e.state);
        std::string base = e.name;  // part file name of another file - the instance name without " <N>"
        if (const auto lt = base.rfind(" <"); lt != std::string::npos && !base.empty() && base.back() == '>') base = base.substr(0, lt);
        for (size_t i = 0; i < e.state->bodies.size(); ++i) {
            Piece p;
            p.name = e.state->bodies.size() > 1 ? chain + " " + e.state->bodies[i].id : chain;
            p.state = e.state, p.body = int(i), p.motion = e.world, p.faceColors = fc, p.path = e.path;
            if (pe) p.color = partColor(*pe);
            p.partKey = e.file + "#" + e.element;
            p.partFileName = pe ? fileNameOf(*pe) : sanitizeFileName(base);
            p.element = entryElement(d, links, e);
            out.push_back(std::move(p));
        }
    }
    return out;
}

// Fit annotations of a face of body bi resolved in the state (printprep.md §5): FaceIdx and limit deviations.
std::vector<printprep::FitTarget> fitTargets(const kernel::Kernel& k, const doc::Element& el, const regen::PartState& st, int bi) {
    std::vector<printprep::FitTarget> out;
    auto deviations = [](const doc::Json& t, double nominal, printprep::FitTarget& x) {
        if (t.contains("field")) {
            const auto d = pmi::fieldDeviations(t["field"].get<std::string>(), nominal);
            if (!d) return false;  // the annotation has an error status — not applied
            x.upper = d->upper, x.lower = d->lower;
        } else {
            x.upper = t["upper"].get<double>(), x.lower = t["lower"].get<double>();
        }
        return true;
    };
    // The hole diameter tolerance (FX12-PMI-007, pmi.md §2.2) — the tolerance of its wall F.side(s1/…), before a face
    // designation on the same face.
    std::set<int> covered;
    for (const auto& fe : st.names.faces) {
        if (fe.body != bi) continue;
        const auto dot = fe.base.find(".side(s1/");
        if (dot == std::string::npos) continue;
        const auto f = el.find(fe.base.substr(0, dot));
        const doc::Json* tol = f && f->type == doc::FeatureType::Hole && !f->suppressed ? doc::featureTolerance(*f, "diameter") : nullptr;
        if (!tol) continue;
        const auto si = k.faceSurface(st.bodies.at(size_t(bi)).shape, fe.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) continue;
        printprep::FitTarget t;
        t.id = f->id;
        t.face = fe.face;
        if (!deviations(*tol, 2 * si.radius, t)) continue;
        covered.insert(fe.face);
        out.push_back(t);
    }
    for (const auto& a : el.annotations) {
        if (a["type"] != "fit" || !a.contains("face")) continue;
        const auto rr = naming::resolve(a["face"].get<std::string>(), naming::EntityKind::Face, st.names);
        if (naming::refFate(rr, true) != naming::RefFate::Resolved) continue;
        const auto& fe = st.names.faces.at(rr.entities.front().index);
        if (fe.body != bi || covered.count(fe.face)) continue;
        const auto si = k.faceSurface(st.bodies.at(size_t(bi)).shape, fe.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) continue;  // PMI_NOT_CYLINDER — not applied
        printprep::FitTarget t;
        t.id = a["id"].get<std::string>();
        t.face = fe.face;
        if (!deviations(a, 2 * si.radius, t)) continue;
        out.push_back(t);
    }
    return out;
}

// Print body of a piece (core-api §4.7а item 2а): the state with the body replaced and its names dropped.
Result<regen::StatePtr> printState(const kernel::Kernel& k, const Piece& p, const printprep::Options& po) {
    const regen::PartState& st = *p.state;
    const kernel::Shape& body = st.bodies.at(size_t(p.body)).shape;
    std::vector<printprep::ThreadTarget> threads;
    for (const auto& t : st.threads) {
        printprep::ThreadTarget x{t.feature, t.internal, t.origin, t.dir, t.radius, t.d, t.pitch, t.length, t.height};
        if (printprep::threadOnBody(k, body, x)) threads.push_back(x);
    }
    const auto fits = p.element ? fitTargets(k, *p.element, st, p.body) : std::vector<printprep::FitTarget>{};
    auto pr = printprep::prepareBody(k, body, fits, threads, po);
    if (!pr) return pr.error();
    auto ns = std::make_shared<regen::PartState>(st);
    ns->bodies[size_t(p.body)].shape = pr->shape;
    ns->names = naming::NameTable{};  // print body faces have no stable names (mesh names are service ones)
    return regen::StatePtr(ns);
}

// Print pieces (core-api §4.7а item 2а): each (state, body) pair is prepared once; face colors are not carried over.
Result<std::vector<Piece>> printPieces(const kernel::Kernel& k, std::vector<Piece> pieces, const printprep::Options& po) {
    std::map<std::pair<const regen::PartState*, int>, regen::StatePtr> done;
    for (auto& p : pieces) {
        const auto key = std::make_pair(p.state.get(), p.body);
        auto it = done.find(key);
        if (it == done.end()) {
            auto s = printState(k, p, po);
            if (!s) return s.error();
            it = done.emplace(key, *s).first;
        }
        p.state = it->second;
        p.faceColors.clear();
    }
    return pieces;
}

Result<std::vector<io::ExportMesh>> meshesOf(const kernel::Kernel& k, const std::vector<Piece>& pieces, const Session::MeshExportOptions& o,
                                             bool world) {
    BBox3 bb;
    bb.setEmpty();
    for (const auto& p : pieces) {
        const BBox3 b = k.bbox(p.state->bodies[size_t(p.body)].shape);
        if (b.isEmpty()) continue;
        for (int c = 0; c < 8; ++c) bb.extend(Point3(p.motion.apply(b.corner(BBox3::CornerType(c)))));
    }
    io::StlOptions so;
    so.chordalDeviation = o.chordalDeviation;
    so.angularDeviation = o.angularDeviation;
    const kernel::TessParams tp = io::tessParams(so, bb);
    std::vector<io::ExportMesh> out;
    for (const auto& p : pieces) {
        auto m = bodyMesh(k, *p.state, p.body, tp);
        if (!m) return m.error();
        if (!m->failedFaces.empty()) return Error("EXPORT_TESSELLATION").with("id", m->bodyId).with("ids", m->failedFaces);
        io::ExportMesh e;
        e.name = p.name;
        e.mesh = std::move(*m);
        if (world && !p.path.empty()) e.mesh.bodyId = p.path + "/" + e.mesh.bodyId;  // assembly: "<path>/<bodyId>" (io.md §2а)
        if (world) {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) e.rotation[size_t(3 * i + j)] = p.motion.R(i, j);
            e.translation = {p.motion.t.x(), p.motion.t.y(), p.motion.t.z()};
        }
        e.color = p.color;
        e.faceColors = p.faceColors;
        out.push_back(std::move(e));
    }
    return out;
}

// Sheet metal part of the export (core-api §4.7е item 2): its state, key, name and file name.
struct FlatSource {
    std::string key, name, fileName;
    regen::StatePtr state;
};

bool hasSheetBody(const regen::PartState& st) { return st.sheet && st.sheet->model && !st.sheet->body.empty() && !st.sheet->flat.null(); }

Status checkFlatOptions(const Session::FlatExportOptions& o) {
    std::set<std::string> seen;
    for (const auto& it : o.items)
        if (!seen.insert(it).second) return Error("EXPORT_INVALID_OPTIONS").with("name", std::string("items")).with("actual", o.items);
    return okStatus();
}

// Sheet metal parts of the object (core-api §4.7е item 2); r — regeneration result of the active element of d.
Result<std::vector<FlatSource>> collectFlatParts(const doc::Document& d, const regen::PartResult& r, const Session::FlatExportOptions& o) {
    std::vector<FlatSource> all;
    const doc::Element& el = d.active();
    if (!el.isAssembly()) {
        if (!r.final) return Error("API_STATE_UNAVAILABLE");
        if (hasSheetBody(*r.final)) all.push_back({el.id, el.name, fileNameOf(el), r.final});
    } else {
        if (!r.assembly) return Error("API_STATE_UNAVAILABLE");
        for (const auto& e : r.assembly->instances) {
            if (!e.state || e.suppressed || e.error || e.kind == "assembly" || !hasSheetBody(*e.state)) continue;
            bool off = false;  // suppressed ancestor
            for (size_t p = e.path.find('/'); p != std::string::npos; p = e.path.find('/', p + 1))
                if (const auto* a = r.assembly->find(e.path.substr(0, p)); a && (a->suppressed || a->error)) off = true;
            if (off) continue;
            const std::string key = e.file + "#" + e.element;
            if (std::any_of(all.begin(), all.end(), [&](const FlatSource& f) { return f.key == key; })) continue;
            const doc::Element* pe = nullptr;
            if (e.file.empty())
                if (const int ei = d.elementIndex(e.element); ei >= 0) pe = &d.elements[size_t(ei)];
            std::string base = e.name;  // part of another file — the instance name without " <N>"
            if (const auto lt = base.rfind(" <"); lt != std::string::npos && !base.empty() && base.back() == '>') base = base.substr(0, lt);
            all.push_back({key, pe ? pe->name : base, pe ? fileNameOf(*pe) : sanitizeFileName(base), e.state});
        }
    }
    for (const auto& it : o.items)
        if (std::none_of(all.begin(), all.end(), [&](const FlatSource& f) { return f.key == it; })) return notFound(it);
    std::vector<FlatSource> out;
    for (auto& f : all)
        if (o.items.empty() || std::find(o.items.begin(), o.items.end(), f.key) != o.items.end()) out.push_back(std::move(f));
    if (out.empty()) return Error("EXPORT_NO_SHEET");
    return out;
}

// Flat pattern of a part as contours and bend lines (core-api §4.7е item 3, io.md §2д.1).
Result<io::FlatDxfPart> flatPartOf(const kernel::Kernel& k, const FlatSource& f) {
    const auto& sh = *f.state->sheet;
    auto fp = sheet::flatPattern(k, *sh.model, sh.flat);
    if (!fp) return fp.error();
    auto loops = io::flatLoops(fp->curves);
    if (!loops) return Error(loops.error()).with("name", f.name);
    io::FlatDxfPart p;
    p.name = f.name;
    p.loops = std::move(*loops);
    for (const auto& b : fp->bends) p.bends.push_back({b.a, b.b, b.up});
    return p;
}

}  // namespace

Result<doc::Document> Session::exportDocument(const std::string& element) const {
    doc::Document d = doc();
    if (!element.empty()) {
        if (d.elementIndex(element) < 0) return notFound(element);
        d.activeElement = element;
    }
    return d;
}

Result<PartResultPtr> Session::elementResult(const doc::Document& d) {
    PartResultPtr pr;
    if (d.activeElement == doc().activeElement) pr = regenResult();
    if (pr && pr->revision == revision_) return pr;
    regen::Regenerator rg(k_);  // not regen_: that one belongs to the worker thread
    auto r = rg.regenerate(d, revision_, nullptr, false, &links_, selfPath());
    if (!r) return Error("REGEN_CANCELLED");
    return PartResultPtr(std::make_shared<const regen::PartResult>(std::move(*r)));
}

ExportId Session::enqueueExportJob(std::function<Status(const Cancel*)> run) {
    const ExportId id = "x" + std::to_string(nextExport_++);
    Job job;
    job.kind = Job::Kind::Export;  // after already queued jobs; later commands do not cancel it (§4.7 item 2)
    job.cancel = std::make_shared<Cancel>();
    job.run = [this, id, run = std::move(run)](const Cancel* c) {
        const Status s = run(c);
        if (cb_.onExportFinished) cb_.onExportFinished(id, s);
    };
    runner_.enqueue(std::move(job));
    return id;
}

ExportId Session::enqueueExport(doc::Document doc0, ExportWrite write) {
    auto d = std::make_shared<const doc::Document>(std::move(doc0));
    const bool activeObject = d->activeElement == doc().activeElement;
    const uint64_t rev = revision_;
    auto links = std::make_shared<const regen::LinkMap>(links_);
    return enqueueExportJob([this, d, rev, activeObject, links, self = selfPath(), write = std::move(write)](const Cancel* c) -> Status {
        PartResultPtr pr;
        if (activeObject) {
            std::lock_guard lk(mu_);
            if (published_ && published_->revision == rev) pr = published_;
        }
        if (!pr) {
            auto r = regen_.regenerate(*d, rev, c, false, links.get(), self);
            if (!r) return Error("REGEN_CANCELLED");
            pr = std::make_shared<const regen::PartResult>(std::move(*r));
        }
        return write(*d, *pr, *links);
    });
}

std::string Session::exportFileName(const std::string& element, const std::string& sheet) const {
    const auto info = document();
    const int i = info.document->elementIndex(element);
    if (i < 0) return "export";
    const doc::Element& el = info.document->elements[size_t(i)];
    // Specification sheet (core-api §4.7в): with several sheets - plus "Sheet N".
    if (el.isSpecification() && !sheet.empty()) {
        const int page = drawing::specPageIndex(sheet);
        const auto pages = drawing::specificationPages(specContent(el));
        if (page >= 0 && page < int(pages.size()) && pages.size() > 1)
            return sanitizeFileName((el.designation ? *el.designation + " " + el.name : el.name) + " Лист " + std::to_string(page + 1));
    }
    // Drawing sheet (core-api §4.7в): with several sheets - plus the sheet name.
    if (const int si = el.sheetIndex(sheet); !sheet.empty() && si >= 0 && el.sheets.size() > 1)
        return sanitizeFileName((el.designation ? *el.designation + " " + el.name : el.name) + " " + el.sheets[size_t(si)].name);
    return fileNameOf(el);
}

Result<ExportId> Session::exportSheet(const std::string& sheetId, const std::string& p, io::SheetFileFormat format) {
    // core-api §4.7в: primitives - synchronously from the current revision, file writing - in a worker thread job.
    auto g = sheetGraphics(sheetId);
    if (!g) return g.error();
    auto graphics = std::make_shared<const drawing::SheetGraphics>(std::move(*g));
    return enqueueExportJob([graphics, p, format](const Cancel*) { return io::writeSheetFile(p, *graphics, format); });
}

Result<ExportId> Session::exportDrawingsPdf(const std::vector<std::string>& drawings, const std::string& p) {
    auto pages = drawingSheets(drawings);
    if (!pages) return pages.error();
    auto graphics = std::make_shared<const std::vector<drawing::SheetGraphics>>(std::move(*pages));
    return enqueueExportJob([graphics, p](const Cancel*) { return io::writeSheetsPdf(p, *graphics); });
}

Result<ExportId> Session::exportDrawingsDxf(const std::vector<std::string>& drawings, const std::string& p) {
    auto pages = drawingSheets(drawings);
    if (!pages) return pages.error();
    auto graphics = std::make_shared<const std::vector<drawing::SheetGraphics>>(std::move(*pages));
    return enqueueExportJob([graphics, p](const Cancel*) { return io::writeSheetsDxf(p, *graphics); });
}

Result<drawing::TextOutline> Session::textOutline(const drawing::Primitive& text) { return drawing::textOutline(text); }

std::optional<std::array<Point2, 4>> Session::sheetTextFrame(const doc::SheetText& text, size_t lineCount) {
    return drawing::sheetTextFrame(text, lineCount);
}

Result<std::vector<io::MeshCheck>> Session::meshReport(const MeshExportOptions& options) {
    if (auto s = checkOptions(options); !s) return s.error();
    auto d = exportDocument(options.element);
    if (!d) return d.error();
    auto pr = elementResult(*d);
    if (!pr) return pr.error();
    auto pieces = collectPieces(*d, **pr, options, &links_);
    if (!pieces) return pieces.error();
    if (options.print) pieces = printPieces(k_, std::move(*pieces), *options.print);
    if (!pieces) return pieces.error();
    auto meshes = meshesOf(k_, *pieces, options, true);
    if (!meshes) return meshes.error();
    return io::checkMeshes(*meshes);
}

Result<ExportId> Session::exportMeshes(const std::string& p, const MeshExportOptions& options) {
    if (auto s = checkOptions(options); !s) return s.error();
    auto d = exportDocument(options.element);
    if (!d) return d.error();
    return enqueueExport(std::move(*d), [this, p, options](const doc::Document& snap, const regen::PartResult& pr,
                                                           const regen::LinkMap& links) -> Status {
        auto pieces = collectPieces(snap, pr, options, &links);
        if (!pieces) return pieces.error();
        if (options.print) pieces = printPieces(k_, std::move(*pieces), *options.print);
        if (!pieces) return pieces.error();
        const io::MeshFileOptions fo{options.fileFormat, options.stlFormat};
        if (!options.perPart) {
            auto meshes = meshesOf(k_, *pieces, options, true);
            if (!meshes) return meshes.error();
            return io::writeMeshFile(p, *meshes, fo);
        }
        // File per part (core-api §4.7а item 4): bodies of the first instance of each part, in part coordinates.
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::u8path(p), ec))
            return Error("IO_WRITE").with("detail", std::string("not a directory")).with("file", p);
        std::vector<std::string> order;
        std::map<std::string, std::vector<Piece>> byPart;
        std::map<std::string, regen::StatePtr> firstState;
        for (const auto& pc : *pieces) {
            if (!byPart.count(pc.partKey)) order.push_back(pc.partKey), firstState[pc.partKey] = pc.state;
            if (pc.state != firstState[pc.partKey]) continue;
            auto& v = byPart[pc.partKey];
            if (std::any_of(v.begin(), v.end(), [&](const Piece& q) { return q.body == pc.body; })) continue;  // other instance of same part
            v.push_back(pc);
        }
        for (const auto& key : order) {
            auto meshes = meshesOf(k_, byPart[key], options, false);
            if (!meshes) return meshes.error();
            const auto u8 = (std::filesystem::u8path(p) / std::filesystem::u8path(byPart[key].front().partFileName + ".stl")).u8string();
            const std::string file(reinterpret_cast<const char*>(u8.data()), u8.size());
            if (auto s = io::writeMeshFile(file, *meshes, fo); !s) return s;
        }
        return okStatus();
    });
}

Result<std::vector<Session::FlatPartInfo>> Session::flatParts(const FlatExportOptions& options) {
    if (auto s = checkFlatOptions(options); !s) return s.error();
    auto d = exportDocument(options.element);
    if (!d) return d.error();
    auto pr = elementResult(*d);
    if (!pr) return pr.error();
    auto parts = collectFlatParts(*d, **pr, options);
    if (!parts) return parts.error();
    std::vector<FlatPartInfo> out;
    for (const auto& f : *parts) {
        auto p = flatPartOf(k_, f);
        if (!p) return p.error();
        out.push_back({f.key, f.name, int(p->loops.size()), io::flatArea(p->loops)});
    }
    return out;
}

Result<ExportId> Session::exportFlatDxf(const std::string& p, const FlatExportOptions& options) {
    if (auto s = checkFlatOptions(options); !s) return s.error();
    auto d = exportDocument(options.element);
    if (!d) return d.error();
    return enqueueExport(std::move(*d), [this, p, options](const doc::Document& snap, const regen::PartResult& pr,
                                                           const regen::LinkMap&) -> Status {
        auto sources = collectFlatParts(snap, pr, options);
        if (!sources) return sources.error();
        std::vector<io::FlatDxfPart> parts;
        for (const auto& f : *sources) {
            auto fp = flatPartOf(k_, f);
            if (!fp) return fp.error();
            parts.push_back(std::move(*fp));
        }
        if (!options.perPart) return io::writeFlatDxf(p, parts, options.bendLines);
        // File per part (core-api §4.7е item 4).
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::u8path(p), ec))
            return Error("IO_WRITE").with("detail", std::string("not a directory")).with("file", p);
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto u8 = (std::filesystem::u8path(p) / std::filesystem::u8path((*sources)[i].fileName + ".dxf")).u8string();
            const std::string file(reinterpret_cast<const char*>(u8.data()), u8.size());
            if (auto s = io::writeFlatDxf(file, {parts[i]}, options.bendLines); !s) return s;
        }
        return okStatus();
    });
}

}  // namespace construe::api
