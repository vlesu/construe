// CoreApi: session of a single document (spec/core-api.md).
#include "api/session.h"
#include "sketch/offset.h"
#include "regen/hole_tables.h"
#include <Eigen/Eigenvalues>

#include "sketch/trim.h"

#include "api/errors.h"
#include "api/variant_commands.h"
#include "api/feature_commands.h"
#include "api/owned_cuts.h"
#include "api/sections.h"
#include "regen/mates.h"
#include "font/font.h"
#include "api/sketch_commands.h"
#include "api/sketch_groups.h"
#include "sheet/sheet.h"

#include "base/ids.h"
#include "base/files.h"
#include "base/strings.h"
#include "base/number_format.h"
#include "doc/schema.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace construe::api {

namespace {

bool isValidVariableName(const std::string& n) {
    static const std::regex re("[A-Za-z_][A-Za-z0-9_]*");
    return std::regex_match(n, re);
}

// Dimension values for the "has it changed" comparison (§4.6, coordinate write rule).
std::map<std::string, std::string> valueSignature(const sketch::Values& v) {
    std::map<std::string, std::string> out;
    for (const auto& [id, r] : v) out[id] = r.ok() ? canonicalNumber(*r) : "E:" + r.error().code;
    return out;
}

bool writesCoordinates(sketch::SketchStatus s) {
    return s == sketch::SketchStatus::UnderConstrained || s == sketch::SketchStatus::FullyConstrained ||
           s == sketch::SketchStatus::OverConstrainedRedundant;
}

}  // namespace

// ---------------------------------------------------------------- StateRef

Result<StateRef> StateRef::fromJson(const Json& j) {
    if (j.is_string()) {
        if (j == "initial") return StateRef::initial();
        if (j == "final") return StateRef::final();
        if (j == "flat") return StateRef::flat();
    } else if (j.is_object() && j.size() == 1) {
        const auto& [k, v] = *j.items().begin();
        if (v.is_string()) {
            if (k == "after") return StateRef{Kind::After, v.get<std::string>()};
            if (k == "preview") return StateRef{Kind::Preview, v.get<std::string>()};
            if (k == "previewTool") return StateRef{Kind::PreviewTool, v.get<std::string>()};
        }
    }
    return Error("API_INVALID_ARGUMENT").with("name", std::string("stateRef"));
}

// ---------------------------------------------------------------- lifecycle

Session::Session(const kernel::Kernel& k, Callbacks callbacks) : k_(k), cb_(std::move(callbacks)), regen_(k) {
    installWeldModelBodies();  // weld bodies on assembly drawing views (FX9-WLD-002)
    installDrawingSectionCutter();  // model sections on drawing views (S7F-DRW-005)
    undo_ = std::make_unique<doc::UndoStack>(doc::Snapshot{doc::createNew(), "", std::nullopt});
    undo_->markSaved();  // a new document is not modified
    revision_ = 1;
    currentRevision_ = 1;
    if (const char* e = std::getenv("CONSTRUE_TEST_REGEN_DELAY_MS")) regenDelayMs_.store(std::atoi(e));  // tests only (§3, FX14-QA-001)
    scheduleRegen();
}

Session::Session(std::unique_ptr<kernel::Kernel> owned, Callbacks callbacks) : Session(*owned, std::move(callbacks)) {
    ownedKernel_ = std::move(owned);  // the kernel object does not move: reference k_ stays valid
}

Session::~Session() {
    {
        std::lock_guard lk(mu_);
        if (regenCancel_) regenCancel_->requested = true;
        if (previewCancel_) previewCancel_->requested = true;
    }
    runner_.stop();  // before destroying the fields that jobs use
}

void Session::reset(doc::Document d, bool fromFile) {
    undo_ = std::make_unique<doc::UndoStack>(doc::Snapshot{std::move(d), "", std::nullopt});
    undo_->markSaved();
    ctx_.reset();
    drag_.reset();
    openSketch_.reset();
    groupDepth_ = 0;
    groupPushed_ = false;
    {
        std::lock_guard lk(mu_);
        preview_.reset();
        if (previewCancel_) previewCancel_->requested = true;
    }
    tessCache_.clear();
    projCache_.clear();
    {
        std::lock_guard lk(mu_);
        drawingModels_.reset();
        ghostStates_.clear();  // regen.md §8а
    }
    links_.clear();  // linked files are read anew for the new document (assembly.md §5а)
    linkStats_.clear();
    ++revision_;
    connectionEditing_.reset();  // FX7-ASM-004: editing ends with any change
    cancelPreview();  // preview over an outdated revision is unavailable (§4.5)
    if (cb_.onDocumentChanged) cb_.onDocumentChanged(revision_);
    // The in-memory cache (entries of the previous document) is cleared; an opened file — from the disk cache (§4.1ж).
    scheduleRegen(/*clearCache*/ true, fromFile && !cacheDir_.empty() && path_.has_value());
}

// ---------------------------------------------------------------- linked files (assembly.md §5а)

std::string Session::selfPath() const { return path_ ? absoluteU8(*path_) : std::string(); }

Session::LinkStat Session::statOf(const std::string& abs) {
    namespace fs = std::filesystem;
    LinkStat st;
    std::error_code ec;
    const fs::path p = fs::u8path(abs);
    st.exists = fs::is_regular_file(p, ec);
    if (st.exists) {
        st.size = fs::file_size(p, ec);
        st.mtime = fs::last_write_time(p, ec).time_since_epoch().count();
    }
    return st;
}

void Session::loadLinks(const doc::Document* root) {
    // Traversal of the document and all linked ones: files reachable via file references are read once.
    std::vector<std::pair<const doc::Document*, std::string>> queue{{root ? root : &doc(), selfPath()}};
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const auto [D, file] = queue[qi];
        const std::string dir = regen::parentDir(file);
        std::vector<std::string> refs;  // file references: assembly instances and drawing views (drawing.md §6.3)
        for (const auto& el : D->elements) {
            for (const auto& in : el.instances) refs.push_back(in.refFile);
            for (const auto& sh : el.sheets)
                for (const auto& v : sh.views) refs.push_back(v.refFile);
        }
        for (const auto& refFile : refs) {
            if (refFile.empty()) continue;
            const std::string abs = regen::linkPath(dir, refFile);
            if (links_.count(abs)) continue;
            regen::LinkedDoc ld;
            if (doc::isLibraryRef(refFile)) {  // a library part copy of document D (library.md §6.3); not a file on disk
                const doc::LibraryPart* lp = D->libraryPart(refFile.substr(std::char_traits<char>::length(doc::LIBRARY_REF_PREFIX)));
                if (!lp) continue;  // impossible in a valid document (document.md §5); the instance gets ASM_FILE_NOT_FOUND
                ld.keep = lp->document;
                ld.doc = lp->document.get();
                links_[abs] = ld;
                continue;
            }
            linkStats_[abs] = statOf(abs);
            std::ifstream f(std::filesystem::u8path(abs), std::ios::binary);
            if (!f) {
                ld.error = Error("ASM_FILE_NOT_FOUND");
            } else {
                std::stringstream ss;
                ss << f.rdbuf();
                auto pd = doc::parse(ss.str());
                if (!pd) {
                    ld.error = Error("ASM_FILE_INVALID").with("detail", pd.error().code);
                } else {
                    auto keep = std::make_shared<const doc::Document>(std::move(*pd));
                    ld.doc = keep.get();
                    ld.keep = keep;
                }
            }
            links_[abs] = ld;
            if (ld.doc) queue.push_back({static_cast<const doc::Document*>(ld.doc), abs});
        }
    }
}

std::vector<std::string> Session::linkChanges() const {
    std::vector<std::string> out;
    for (const auto& [abs, st] : linkStats_) {
        const LinkStat now = statOf(abs);
        if (now.exists != st.exists || now.size != st.size || now.mtime != st.mtime) out.push_back(abs);
    }
    return out;
}

std::vector<std::pair<std::string, std::optional<Error>>> Session::linkedFiles() const {
    std::vector<std::pair<std::string, std::optional<Error>>> out;
    for (const auto& [abs, ld] : links_)
        if (!doc::isLibraryRef(abs)) out.push_back({abs, ld.error});  // library part copies are not files (document.md §2.13)
    return out;
}

Result<std::vector<Session::ElementSummary>> Session::fileElements(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return Error("IO_READ").with("detail", std::string("cannot open")).with("file", path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto d = doc::parse(ss.str());
    if (!d) return d.error();
    std::vector<ElementSummary> out;
    for (const auto& e : d->elements) out.push_back({e.id, e.name, e.kind});
    return out;
}

Result<std::vector<std::string>> Session::refreshLinks() {
    if (auto s = checkNoDrag(); !s) return s.error();
    const auto changed = linkChanges();
    for (const auto& abs : changed) links_.erase(abs), linkStats_.erase(abs);
    tessCache_.clear();
    scheduleRegen(false);  // re-reading happens in scheduleRegen (loadLinks); document, revision and undo do not change
    return changed;
}

// ---------------------------------------------------------------- worker thread (§3)

bool Session::waitIdle(int timeoutMs) { return runner_.waitIdle(timeoutMs); }

void Session::publish(PartResultPtr r, std::shared_ptr<const regen::CacheEntries> entries,
                      std::map<std::string, regen::StatePtr> ghosts) {
    {
        std::lock_guard lk(mu_);
        if (r->revision != currentRevision_) return;
        published_ = r;
        for (auto& [id, st] : ghosts) ghostStates_[id] = std::move(st);  // regen.md §8а
        publishedEntries_ = std::move(entries);
        stats_ = regen_.stats();
        timings_ = regen_.timings();
        if (!r->models.empty()) drawingModels_ = std::make_shared<const std::map<std::string, regen::DrawingModel>>(r->models);
    }
    if (cb_.onRegenerated) cb_.onRegenerated(r);
}

void Session::scheduleRegen(bool clearCache, bool diskCache) {
    loadLinks();  // linked files (assembly.md §5а): missing ones are read on the owner thread
    auto links = std::make_shared<const regen::LinkMap>(links_);
    const std::string self = selfPath();
    // Disk cache (§4.1ж): the key is by the document and linked files, computed on the owner thread.
    const std::string cacheDir = diskCache ? cacheDir_ : std::string();
    const uint64_t cacheMax = cacheMaxBytes_;
    auto d = std::make_shared<const doc::Document>(regenDocument());  // core-api §4.1и: an edited connection is suppressed
    const uint64_t cacheKey = diskCache ? documentCacheKey(k_, *d, links_, self) : 0;
    const uint64_t rev = revision_;
    auto cancel = std::make_shared<Cancel>();
    {
        std::lock_guard lk(mu_);
        currentRevision_ = rev;
        if (regenCancel_) regenCancel_->requested = true;
        regenCancel_ = cancel;
    }
    Job job;
    job.kind = Job::Kind::Regen;
    job.cancel = cancel;
    job.run = [this, d, rev, clearCache, links, self, cacheDir, cacheMax, cacheKey](const Cancel* c) {
        if (cancelled(c)) return;
        // Slow regeneration for tests (§3, FX14-QA-001): waited in short steps, a cancelled job stops waiting.
        for (int left = regenDelayMs_.load(); left > 0 && !cancelled(c); left -= 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(left, 10)));
        if (cancelled(c)) return;
        if (clearCache) regen_.clearCache();
        bool loaded = false;
        if (!cacheDir.empty())
            if (auto entries = loadCacheFile(k_, cacheDir, cacheKey)) {
                regen_.preload(*entries);
                loaded = true;
            }
        if (cb_.onRegenStarted) cb_.onRegenStarted(rev);
        const long long evals0 = regen_.evaluations();
        regen_.startRecording();
        auto r = regen_.regenerate(*d, rev, c, true, links.get(), self);
        auto entries = std::make_shared<const regen::CacheEntries>(regen_.stopRecording());
        if (!r) return;
        if (d->active().isDrawing() && !precomputeDrawing(*d, *r, self, c)) return;  // view projections pre-publish (§5.4)
        const bool missed = regen_.evaluations() != evals0;
        auto ghosts = referenceStates(*d, *r);
        publish(std::make_shared<const regen::PartResult>(std::move(*r)), entries, std::move(ghosts));
        // The file did not exist, is invalid, or there were computations — write the used entries (§4.1ж).
        if (!cacheDir.empty() && (!loaded || missed)) storeCacheFile(k_, cacheDir, cacheKey, *entries, cacheMax);
    };
    runner_.enqueue(std::move(job));
}

// ---------------------------------------------------------------- common parts of commands

Status Session::checkNoDrag() const {
    if (drag_) return apiError("API_DRAG_ACTIVE");
    return okStatus();
}

void Session::resolveSketches(doc::Document& nd, const doc::Document& before, const std::vector<std::string>& touched) {
    const auto unitsAfter = regen::documentUnits(nd), unitsBefore = regen::documentUnits(before);
    const auto varsAfter = regen::documentVariables(nd), varsBefore = regen::documentVariables(before);
    for (size_t i = 0; i < nd.active().features.size(); ++i) {
        const doc::FeaturePtr f = nd.active().features[i];
        const auto* s = f->sketchData();
        if (!s) continue;
        const std::string path = "/elements/" + std::to_string(nd.activeIndex()) + "/features/" + std::to_string(i);
        const auto values = regen::dimensionValues(*s, varsAfter, unitsAfter, path);
        bool affected = std::find(touched.begin(), touched.end(), f->id) != touched.end();
        if (!affected) {
            const doc::FeaturePtr old = before.active().find(f->id);
            if (old && old->sketchData()) {
                const auto oldValues = regen::dimensionValues(*old->sketchData(), varsBefore, unitsBefore, path);
                affected = valueSignature(oldValues) != valueSignature(values);
            }
        }
        if (!affected) continue;
        // Projections are needed but unavailable — the sketch is not solved, coordinates do not change (Р-018).
        const auto sd = withProjections(nd, *f);
        if (!sd) continue;
        // Holding by weak dimensions (sketch.md §4.4 item 3, S4-WEAK-003): the underdefined part stays where it was.
        const sketch::SketchResult r = sketch::solveHeld(*sd, values);
        if (!writesCoordinates(r.status)) continue;
        doc::Feature nf = *f;
        nf.data = sketch::applySolution(*s, r.state);
        nd.active().features[i] = std::make_shared<doc::Feature>(std::move(nf));
    }
}

Status Session::commit(doc::Document nd, const std::string& labelKey, std::vector<std::string> touched,
                       const std::string& subject) {
    resolveSketches(nd, doc(), touched);
    doc::normalizeElementGroups(nd);  // document.md §2.1а: members contiguous, no empty groups
    if (doc::serialize(nd) == doc::serialize(doc())) return okStatus();
    // A new context reference must not create a cycle between parts (assembly.md §7в.4).
    for (const auto& el : nd.elements) {
        if (el.isAssembly() || !el.context || el.context->frozen) continue;
        const int oi = doc().elementIndex(el.id);
        const bool changed = oi < 0 || !(doc().elements[size_t(oi)].context == el.context) ||
                             regen::contextRefs(doc().elements[size_t(oi)]) != regen::contextRefs(el);
        if (changed && regen::contextDependsOn(nd, el.id, el.id)) return Error("API_CONTEXT_CYCLE").with("id", el.id);
    }
    // Name of the affected object: in the new document, and for a deleted one — in the previous one.
    std::string name;
    for (const doc::Document* d : std::array<const doc::Document*, 2>{&nd, &doc()}) {
        if (subject.empty() || !name.empty()) break;
        if (const auto f = d->active().find(subject)) name = f->name;
        for (const auto& el : d->elements)
            if (el.id == subject) name = el.name;
        for (const auto& v : d->variables)
            if (v.id == subject) name = v.name;
        for (const auto& t : d->variantTables)
            if (t.id == subject) name = t.name;
        for (const auto& sec : d->active().sections)
            if (sec.id == subject) name = sec.name;
        for (const auto& in : d->active().instances)  // assembly instances (insertLibraryPart, seatLibraryPart)
            if (in.id == subject) name = in.name;
        for (const auto& m : d->active().mates)  // assembly mates (rebindReferences, S7F-FEAT-003)
            if (m.id == subject) name = m.name;
        for (const auto& w : d->active().welds)  // assembly weld seams (§4.1в)
            if (w.value("id", std::string()) == subject) name = w.value("name", std::string());
        for (const auto& p : d->active().patterns)  // assembly instance patterns (§4.1в, FX7-ASM-007)
            if (p.id == subject) name = p.name;
        for (const auto& sh : d->active().sheets) {  // drawing sheets and views (§4.1г)
            if (sh.id == subject) name = sh.name;
            for (const auto& v : sh.views)
                if (v.id == subject) name = v.name;
        }
    }
    doc::Snapshot s{std::move(nd), groupDepth_ > 0 ? groupLabel_ : labelKey, ctx_, name};
    if (groupDepth_ > 0 && groupPushed_) {
        undo_->replaceCurrent(std::move(s));
    } else {
        undo_->push(std::move(s));
        if (groupDepth_ > 0) groupPushed_ = true;
    }
    ++revision_;
    connectionEditing_.reset();  // FX7-ASM-004: editing ends with any change
    openSketch_.reset();
    cancelPreview();  // preview over an outdated revision is unavailable (§4.5)
    if (cb_.onDocumentChanged) cb_.onDocumentChanged(revision_);
    scheduleRegen();
    return okStatus();
}

// ---------------------------------------------------------------- §4.1

Status Session::newDocument(const std::string& lengthUnit, const std::string& angleUnit) {
    if (groupDepth_ > 0) return apiError("API_GROUP_OPEN");
    if (!expr::parseLengthUnit(lengthUnit)) return Error("API_INVALID_ARGUMENT").with("name", std::string("units.length"));
    if (!expr::parseAngleUnit(angleUnit)) return Error("API_INVALID_ARGUMENT").with("name", std::string("units.angle"));
    reset(doc::createNew(lengthUnit, angleUnit));
    path_.reset();
    return okStatus();
}

Status Session::openDocument(const std::string& p) {
    if (groupDepth_ > 0) return apiError("API_GROUP_OPEN");
    const std::optional<std::string> oldPath = path_;
    namespace fs = std::filesystem;
    std::ifstream f(u8path(p), std::ios::binary);
    if (!f) return Error("IO_READ").with("detail", std::string("cannot open")).with("file", p);
    std::stringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return Error("IO_READ").with("detail", std::string("read failed")).with("file", p);
    auto d = doc::parse(ss.str());
    if (!d) return d.error();
    path_ = p;  // before reset: linked files are resolved relative to the document directory (assembly.md §5а)
    reset(migrateOffsetGroups(std::move(*d)), /*fromFile*/ true);  // offset groups of older versions (FX12-SK-001)
    (void)oldPath;
    return okStatus();
}

Status Session::openRecovery(const std::string& copyPath, const std::optional<std::string>& originalPath) {
    if (auto s = openDocument(copyPath); !s) return s;
    path_ = originalPath;
    undo_->markUnsaved();
    return okStatus();
}

Status Session::saveDocument(const std::optional<std::string>& p) {
    const std::optional<std::string> target = p ? p : path_;
    if (!target) return apiError("API_NO_PATH");
    syncAssemblyPositions();  // solved instance positions — into the file (assembly.md §7.5)
    // Owned cuts of connections "in parts" (solutions.md §7.10 items 5б, 6): after the regeneration of this revision.
    const bool ownedCuts = doc().active().isAssembly();
    if (ownedCuts) {
        runner_.waitIdle(solutions::SOL_SYNC_WAIT_MS);
        syncOwnedCutsFromPublished();
    }
    const std::optional<std::string> oldPath = path_;
    // Version of the program that saved the file (document.md §2); does not affect document meaning.
    // Saving to another directory: relative paths of file references are recomputed (assembly.md §5а).
    if (path_ && absoluteU8(regen::parentDir(absoluteU8(*target))) != absoluteU8(regen::parentDir(absoluteU8(*path_)))) {
        doc::Document nd = doc();
        bool changed = false;
        const std::string oldDir = regen::parentDir(absoluteU8(*path_)), newDir = regen::parentDir(absoluteU8(*target));
        auto rebase = [&](std::string& refFile) {  // instance and drawing view references (drawing.md §6.3)
            if (refFile.empty() || doc::isLibraryRef(refFile)) return;
            const std::string abs = regen::linkPath(oldDir, refFile);
            const std::string r = relativeU8(abs, newDir);
            if (!r.empty() && r != refFile) refFile = r, changed = true;
        };
        for (auto& el : nd.elements) {
            for (auto& in : el.instances) rebase(in.refFile);
            for (auto& sh : el.sheets)
                for (auto& v : sh.views) rebase(v.refFile);
        }
        if (changed) undo_->replaceCurrent(doc::Snapshot{nd, undo_->current().label, ctx_, undo_->current().labelName});
    }
    doc::Document out = doc();
    out.savedBy = doc::SavedBy{"construe", CONSTRUE_VERSION};
    if (auto s = io::writeFileAtomic(*target, doc::serialize(out)); !s) return s;
    path_ = target;
    undo_->markSaved();
    scheduleCacheStore();
    if (ownedCuts) return writeLinkedOwnedCuts(*target, oldPath);  // linked part files (solutions.md §7.10 item 6)
    return okStatus();
}

void Session::scheduleCacheStore() {
    if (cacheDir_.empty() || !path_) return;
    loadLinks();
    const uint64_t key = documentCacheKey(k_, doc(), links_, selfPath());
    const uint64_t rev = revision_;
    Job job;
    job.kind = Job::Kind::Export;  // after already queued jobs; does not cancel regeneration
    job.cancel = std::make_shared<Cancel>();
    job.run = [this, key, rev, dir = cacheDir_, max = cacheMaxBytes_](const Cancel*) {
        std::shared_ptr<const regen::CacheEntries> entries;
        {
            std::lock_guard lk(mu_);
            if (!published_ || published_->revision != rev) return;  // the result of this revision is not published
            entries = publishedEntries_;
        }
        if (entries) storeCacheFile(k_, dir, key, *entries, max);
    };
    runner_.enqueue(std::move(job));
}

Status Session::rebuild() {
    if (auto s = checkNoDrag(); !s) return s;
    scheduleRegen(true);
    return okStatus();
}

Status Session::setUnits(const std::string& lengthUnit, const std::string& angleUnit) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!expr::parseLengthUnit(lengthUnit)) return Error("API_INVALID_ARGUMENT").with("name", std::string("units.length"));
    if (!expr::parseAngleUnit(angleUnit)) return Error("API_INVALID_ARGUMENT").with("name", std::string("units.angle"));
    doc::Document nd = doc();
    // Unitless numbers in length/angle fields are in document units: converted, the model does not change (§4.1).
    const double kl = expr::lengthFactor(*expr::parseLengthUnit(nd.lengthUnit)) / expr::lengthFactor(*expr::parseLengthUnit(lengthUnit));
    const double ka = expr::angleFactor(*expr::parseAngleUnit(nd.angleUnit)) / expr::angleFactor(*expr::parseAngleUnit(angleUnit));
    static const std::regex bare(R"(^\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?\s*$)");
    auto convert = [&](std::string& text, double k) {
        if (k != 1.0 && std::regex_match(text, bare)) text = canonicalNumber(std::stod(text) * k);
    };
    for (auto& el : nd.elements)  // units are shared by the document — fields of all elements are converted
    for (auto& fp : el.features) {
        doc::Feature nf = *fp;
        // Length and angle fields (document.md §2.11); dimensionless ones (pattern instance count) are not converted.
        doc::forEachExpression(nf, [&](std::string& text, doc::ExprDim dim, bool) {
            if (dim == doc::ExprDim::Length) convert(text, kl);
            else if (dim == doc::ExprDim::Angle) convert(text, ka);
        });
        fp = std::make_shared<doc::Feature>(std::move(nf));
    }
    nd.lengthUnit = lengthUnit;
    nd.angleUnit = angleUnit;
    return commit(std::move(nd), "undo.setUnits");
}

Result<ExportId> Session::exportStl(const std::string& p, const io::StlOptions& options) {
    if (auto s = io::validateOptions(options); !s) return s.error();
    return enqueueExport(doc(), [this, p, options](const doc::Document&, const regen::PartResult& pr, const regen::LinkMap&) -> Status {
        const regen::PartState& st = *pr.final;
        std::vector<int> chosen;
        if (options.allBodies) {
            for (size_t i = 0; i < st.bodies.size(); ++i) chosen.push_back(static_cast<int>(i));
        } else {
            for (const auto& bid : options.bodies) {
                auto it = std::find_if(st.bodies.begin(), st.bodies.end(), [&](const regen::Body& b) { return b.id == bid; });
                if (it == st.bodies.end()) return notFound(bid);
                chosen.push_back(static_cast<int>(it - st.bodies.begin()));
            }
            std::sort(chosen.begin(), chosen.end());
        }
        BBox3 bb;
        bb.setEmpty();
        for (int i : chosen) bb.extend(k_.bbox(st.bodies[i].shape));
        const kernel::TessParams tp = io::tessParams(options, bb);
        std::vector<io::BodyMesh> meshes;
        for (int i : chosen) {
            auto m = bodyMesh(k_, st, i, tp);
            if (!m) return m.error();
            if (!m->failedFaces.empty())
                return Error("EXPORT_TESSELLATION").with("id", m->bodyId).with("ids", m->failedFaces);
            meshes.push_back(std::move(*m));
        }
        return io::writeStl(p, meshes, options.format);
    });
}

// ---------------------------------------------------------------- §4.2

namespace {
// Context after undo/redo (§4.2, FX1-SKI-002): an open sketch that exists in the restored snapshot stays
// open; otherwise — the snapshot's context.
std::optional<std::string> contextAfterHistoryStep(const std::optional<std::string>& open, const doc::Snapshot& s) {
    if (open)
        if (const auto f = s.document.active().find(*open); f && f->sketchData()) return open;
    return s.editSketch;
}
}  // namespace

Status Session::undo() {
    if (groupDepth_ > 0) return apiError("API_GROUP_OPEN");
    if (auto s = checkNoDrag(); !s) return s;
    if (!undo_->undo()) return apiError("UNDO_EMPTY");
    ctx_ = contextAfterHistoryStep(ctx_, undo_->current());
    ++revision_;
    connectionEditing_.reset();  // FX7-ASM-004: editing ends with any change
    openSketch_.reset();
    cancelPreview();  // preview over an outdated revision is unavailable (§4.5)
    if (cb_.onDocumentChanged) cb_.onDocumentChanged(revision_);
    scheduleRegen();
    return okStatus();
}

Status Session::redo() {
    if (groupDepth_ > 0) return apiError("API_GROUP_OPEN");
    if (auto s = checkNoDrag(); !s) return s;
    if (!undo_->redo()) return apiError("REDO_EMPTY");
    ctx_ = contextAfterHistoryStep(ctx_, undo_->current());
    ++revision_;
    connectionEditing_.reset();  // FX7-ASM-004: editing ends with any change
    openSketch_.reset();
    cancelPreview();  // preview over an outdated revision is unavailable (§4.5)
    if (cb_.onDocumentChanged) cb_.onDocumentChanged(revision_);
    scheduleRegen();
    return okStatus();
}

std::optional<UndoLabel> Session::undoLabel() const {
    if (!undo_->canUndo()) return std::nullopt;
    const std::string n = undo_->undoLabelName();
    return UndoLabel{undo_->undoLabel(), n.empty() ? std::map<std::string, std::string>{} : std::map<std::string, std::string>{{"name", n}}};
}

std::optional<UndoLabel> Session::redoLabel() const {
    if (!undo_->canRedo()) return std::nullopt;
    const std::string n = undo_->redoLabelName();
    return UndoLabel{undo_->redoLabel(), n.empty() ? std::map<std::string, std::string>{} : std::map<std::string, std::string>{{"name", n}}};
}

void Session::beginGroup(const std::string& labelKey) {
    if (groupDepth_++ == 0) {
        groupLabel_ = labelKey;
        groupPushed_ = false;
    }
}

Status Session::endGroup() {
    if (groupDepth_ == 0) return apiError("API_NO_GROUP");
    if (--groupDepth_ == 0) groupPushed_ = false;
    return okStatus();
}

// ---------------------------------------------------------------- §4.3

Result<std::string> Session::addVariable(const std::string& name, const std::string& expression,
                                         const std::optional<std::string>& comment, const std::string& type,
                                         const std::vector<std::string>& values) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (auto s = checkVariableType(type, values); !s) return s.error();
    if (!isValidVariableName(name)) return Error("API_INVALID_NAME").with("name", name);
    if (expr::isReservedName(name)) return Error("EXPR_RESERVED_NAME").with("name", name);
    for (const auto& v : doc().variables)
        if (v.name == name) return Error("DOC_DUPLICATE_NAME").with("name", name);
    doc::Document nd = doc();
    doc::Variable v;
    v.id = doc::allocate(nd, doc::IdKind::Variable);
    v.name = name;
    v.expr = expression;
    v.comment = comment;
    v.type = type;
    v.values = values;
    if (type == "list" && std::find(values.begin(), values.end(), expression) == values.end()) v.expr = values[0];
    const std::string id = v.id;
    nd.variables.push_back(std::move(v));
    nd = rebuildVariableGroups(std::move(nd));  // sketch groups on variables (sketch.md §2.6.2, S4-SKT-006)
    if (auto s = commit(std::move(nd), "undo.addVariable", {}, id); !s) return s.error();
    return id;
}

Status Session::updateVariable(const std::string& id, const std::optional<std::string>& name,
                               const std::optional<std::string>& expression,
                               const std::optional<std::optional<std::string>>& comment) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto it = std::find_if(nd.variables.begin(), nd.variables.end(), [&](const doc::Variable& v) { return v.id == id; });
    if (it == nd.variables.end()) return notFound(id);
    if (name && *name != it->name) {
        if (!isValidVariableName(*name)) return Error("API_INVALID_NAME").with("name", *name);
        if (expr::isReservedName(*name)) return Error("EXPR_RESERVED_NAME").with("name", *name);
        for (const auto& v : nd.variables)
            if (v.name == *name) return Error("DOC_DUPLICATE_NAME").with("name", *name);
        it->name = *name;
    }
    if (expression && !setVariantCell(nd, id, *expression)) it->expr = *expression;  // column — the active row cell (§4.3)
    if (comment) it->comment = *comment;
    nd = rebuildVariableGroups(std::move(nd));  // S4-SKT-006
    return commit(std::move(nd), "undo.updateVariable", {}, id);
}

Status Session::renameVariableEverywhere(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    const auto cur = std::find_if(doc().variables.begin(), doc().variables.end(), [&](const doc::Variable& v) { return v.id == id; });
    if (cur == doc().variables.end()) return notFound(id);
    const std::string from = cur->name;
    if (name == from) return okStatus();
    doc::Document nd = doc();
    auto it = std::find_if(nd.variables.begin(), nd.variables.end(), [&](const doc::Variable& v) { return v.id == id; });
    if (!isValidVariableName(name)) return Error("API_INVALID_NAME").with("name", name);
    if (expr::isReservedName(name)) return Error("EXPR_RESERVED_NAME").with("name", name);
    for (const auto& v : nd.variables)
        if (v.name == name) return Error("DOC_DUPLICATE_NAME").with("name", name);
    it->name = name;
    auto rename = [&](std::string& t) { t = expr::renameReference(t, from, name); };
    // String values are text, not formulas (§5а): they are not touched.
    std::set<std::string> strings;
    for (auto& v : nd.variables) {
        if (v.type == "string") strings.insert(v.id);
        else rename(v.expr);
    }
    for (auto& t : nd.variantTables)
        for (auto& r : t.variants)
            for (size_t i = 0; i < r.values.size() && i < t.columns.size(); ++i)
                if (!strings.count(t.columns[i])) rename(r.values[i]);
    for (auto& el : nd.elements) {
        for (auto& f : el.features) {
            auto copy = std::make_shared<doc::Feature>(*f);
            bool changed = false;
            doc::forEachExpression(*copy, [&](std::string& t, doc::ExprDim, bool) {
                const std::string r = expr::renameReference(t, from, name);
                if (r != t) t = r, changed = true;
            });
            if (changed) f = copy;
        }
        for (auto& m : el.mates)
            if (m.value) rename(*m.value);
    }
    nd = rebuildVariableGroups(std::move(nd));  // S4-SKT-006
    return commit(std::move(nd), "undo.updateVariable", {}, id);
}

Status Session::deleteVariable(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto it = std::find_if(nd.variables.begin(), nd.variables.end(), [&](const doc::Variable& v) { return v.id == id; });
    if (it == nd.variables.end()) return notFound(id);
    nd.variables.erase(it);
    removeVariantColumn(nd, id);  // §4.3: with its table column
    nd = rebuildVariableGroups(std::move(nd));  // S4-SKT-006
    return commit(std::move(nd), "undo.deleteVariable", {}, id);
}

// ---------------------------------------------------------------- §4.4

namespace {
// Operation of the feature definition (empty — no field): for the default name (document.md §2.12).
std::string definitionOperation(const Json& definition) {
    if (definition.is_object() && definition.contains("operation") && definition["operation"].is_string()) return definition["operation"].get<std::string>();
    return {};
}

// Owned reference features (core-api §4.4, selection.md §2.5, FX7-SEL-001).
std::string ownerOf(const doc::Feature& f) {
    const auto* g = f.refGeom();
    return g && g->def.contains("owner") && g->def["owner"].is_string() ? g->def["owner"].get<std::string>() : std::string();
}

// {"aux": i} objects anywhere in j → {"feature": ids[i]}; false — an index out of range.
bool substituteAux(Json& j, const std::vector<std::string>& ids) {
    if (j.is_object()) {
        if (j.size() == 1 && j.contains("aux")) {
            if (!j["aux"].is_number_integer() || j["aux"].get<long long>() < 0 || j["aux"].get<size_t>() >= ids.size()) return false;
            j = Json{{"feature", ids[j["aux"].get<size_t>()]}};
            return true;
        }
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!substituteAux(it.value(), ids)) return false;
    } else if (j.is_array()) {
        for (auto& x : j)
            if (!substituteAux(x, ids)) return false;
    }
    return true;
}

// Rule В2 items 3–4: features owned by owner that no remaining feature references are removed (repeatedly: an owned
// feature may be referenced only by another owned one).
void dropUnusedOwned(doc::Document& nd, const std::string& owner) {
    auto& feats = nd.active().features;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < feats.size(); ++i) {
            if (ownerOf(*feats[i]) != owner) continue;
            const std::string id = feats[i]->id;
            bool used = false;
            for (const auto& f : feats)
                if (f->id != id && doc::featureDependencies(*f).count(id)) used = true;
            auto& rb = nd.active().rollback;
            if (used) continue;
            if (rb.kind == doc::Rollback::Kind::After && rb.feature == id)
                rb = i == 0 ? doc::Rollback{doc::Rollback::Kind::Start, {}} : doc::Rollback{doc::Rollback::Kind::After, feats[i - 1]->id};
            feats.erase(feats.begin() + long(i));
            changed = true;
            break;
        }
    }
}
}  // namespace

// Owned features of aux inserted at pos of nd (before their consumer `owner`); ids — their allocated IDs.
Status Session::insertAux(doc::Document& nd, const std::vector<Json>& aux, const std::vector<std::string>& ids, const std::string& owner,
                          int pos) {
    for (size_t i = 0; i < aux.size(); ++i) {
        Json ad = aux[i];
        if (!ad.is_object() || !ad.contains("type") || (ad["type"] != "plane" && ad["type"] != "axis"))
            return invalidDefinition("aux type", "/aux/" + std::to_string(i) + "/type");
        if (!substituteAux(ad, ids)) return invalidDefinition("aux index", "/aux/" + std::to_string(i));
        ad["owner"] = owner;
        if (auto st = checkAssemblyFeature(ad); !st) return st;
        const std::string name = doc::defaultFeatureName(nd.active(), featureTypeFromName(ad["type"]), std::string());
        auto fj = featureJsonFromDefinition(ad, ids[i], name, false);
        if (!fj) return fj.error();
        if (auto st = checkDefinitionRefs(nd, ad, ids[i], pos + int(i)); !st) return st;
        auto r = applyFeatureJson(nd, ids[i], *fj, pos + int(i), false);
        if (!r) return r.error();
        nd = std::move(*r);
    }
    return okStatus();
}

Result<std::string> Session::addFeature(const Json& definitionIn, const std::vector<Json>& aux) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (doc().active().isBody() || doc().active().isDrawing() || doc().active().isSpecification())  // body, drawing, specification (document.md §2.4в, §2.4г, §2.4е)
        return Error("API_NOT_PART").with("id", doc().active().id);
    doc::Document nd = doc();
    // Owned features first (lower IDs, right before the feature; core-api §4.4, FX7-SEL-001).
    std::vector<std::string> auxIds;
    for (size_t i = 0; i < aux.size(); ++i) auxIds.push_back(doc::allocate(nd, doc::IdKind::Feature));
    Json definition = definitionIn;
    if (!substituteAux(definition, auxIds)) return invalidDefinition("aux index", "/aux");
    // Assembly: reference geometry, sketches, cut and hole (assembly.md §7б, §7г.5).
    if (auto s = checkAssemblyFeature(definition); !s) return s.error();
    int pos = insertionIndex(nd.active());
    const bool markerAtEnd = nd.active().rollback.kind == doc::Rollback::Kind::End;
    const std::string id = doc::allocate(nd, doc::IdKind::Feature);
    if (auto s = insertAux(nd, aux, auxIds, id, pos); !s) return s.error();
    pos += int(aux.size());
    if (!definition.is_object() || !definition.contains("type") || !definition["type"].is_string())
        return invalidDefinition("type required", "/type");
    if (definition["type"] == "connectionCut") return invalidDefinition("type", "/type");  // created by a connection (solutions.md §7.10)
    const std::string name = doc::defaultFeatureName(nd.active(), featureTypeFromName(definition["type"]), definitionOperation(definition));
    auto fj = featureJsonFromDefinition(definition, id, name, false);
    if (!fj) return fj.error();
    if (auto s = checkDefinitionRefs(nd, definition, id, pos); !s) return s.error();
    auto r = applyFeatureJson(nd, id, *fj, pos, false);
    if (!r) return r.error();
    if (!markerAtEnd) r->active().rollback = {doc::Rollback::Kind::After, id};
    std::vector<std::string> touched;
    if (definition["type"] == "sketch") touched.push_back(id);
    if (auto s = commit(std::move(*r), "undo.addFeature", touched, id); !s) return s.error();
    return id;
}

Status Session::updateFeature(const std::string& id, const Json& definitionIn, const std::vector<Json>& aux) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document d = doc();
    int pos = d.active().indexOf(id);
    if (pos < 0) return notFound(id);
    // Owned features of the new version (core-api §4.4, FX7-SEL-001): inserted right before the feature.
    std::vector<std::string> auxIds;
    for (size_t i = 0; i < aux.size(); ++i) auxIds.push_back(doc::allocate(d, doc::IdKind::Feature));
    Json definition = definitionIn;
    if (!substituteAux(definition, auxIds)) return invalidDefinition("aux index", "/aux");
    if (auto s = insertAux(d, aux, auxIds, id, pos); !s) return s;
    pos += int(aux.size());
    if (auto s = checkOwnedFeature(id, false); !s) return s;  // solutions.md §7.10 item 2
    const doc::Feature& old = *d.active().features[pos];
    // Extrude ↔ cut: the default name follows the operation if the user has not changed it (§4.4, FX1-FEAT-003).
    std::string name = old.name;
    if (const auto* x = old.extrude()) {
        const std::string newOp = definitionOperation(definition);
        auto cutGroup = [](const std::string& op) { return op == "cut" || op == "intersect"; };
        if (!newOp.empty() && cutGroup(newOp) != cutGroup(x->operation)) {
            const std::string base = doc::defaultFeatureBase(old.type, x->operation);
            const bool isDefault = name.size() > base.size() && name.compare(0, base.size(), base) == 0 &&
                                   std::all_of(name.begin() + long(base.size()), name.end(), [](char ch) { return ch >= '0' && ch <= '9'; }) &&
                                   name[base.size()] != '0';
            if (isDefault) name = doc::defaultFeatureName(d.active(), old.type, newOp, id);
        }
    }
    auto fj = featureJsonFromDefinition(definition, id, name, old.suppressed);
    if (!fj) return fj.error();
    if ((*fj)["type"] != doc::featureTypeName(old.type)) return invalidDefinition("type cannot change", "/type");
    if (auto s = checkAssemblyFeature(definition); !s) return s;  // assembly.md §7г.5
    if (old.type == doc::FeatureType::Sketch) {
        const Json oldJ = doc::featureToJsonValue(old);
        if (!fj->contains("entities") || !fj->contains("constraints") || (*fj)["entities"] != oldJ["entities"] ||
            (*fj)["constraints"] != oldJ["constraints"])
            return invalidDefinition("sketch content must not change", "/entities");
    }
    if (auto s = checkDefinitionRefs(d, definition, id, pos); !s) return s;
    auto r = applyFeatureJson(d, id, *fj, pos, true);
    if (!r) return r.error();
    dropUnusedOwned(*r, id);  // rule В2 item 4: owned features the new version no longer references
    std::vector<std::string> touched;
    if (old.type == doc::FeatureType::Sketch) touched.push_back(id);
    return commit(std::move(*r), "undo.updateFeature", touched, id);
}

Status Session::deleteFeature(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto& el = nd.active();
    const int pos = el.indexOf(id);
    if (pos < 0) return notFound(id);
    if (auto s = checkOwnedFeature(id, true); !s) return s;  // solutions.md §7.10 item 2
    // A connection or an assembly hole (assembly.md §7г.6) — the owned features of the parts go too.
    const bool connection = el.isAssembly() && (el.features[size_t(pos)]->type == doc::FeatureType::Connection ||
                                                el.features[size_t(pos)]->type == doc::FeatureType::Hole);
    if (el.rollback.kind == doc::Rollback::Kind::After && el.rollback.feature == id) {
        if (pos == 0) el.rollback = {doc::Rollback::Kind::Start, {}};
        else el.rollback = {doc::Rollback::Kind::After, el.features[pos - 1]->id};
    }
    el.features.erase(el.features.begin() + pos);
    dropUnusedOwned(nd, id);  // rule В2 item 3: its owned features nobody else references (FX7-SEL-001)
    if (ctx_ && *ctx_ == id) ctx_.reset();
    if (connection) materializeConnections(nd, {id});  // its owned cuts go too (solutions.md §7.10 item 5а)
    return commit(std::move(nd), "undo.deleteFeature", {}, id);
}

Status Session::renameFeature(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    const std::string n = trim(name);
    doc::FeaturePtr f = doc().active().find(id);
    if (!f) return notFound(id);
    if (auto s = checkOwnedFeature(id, false); !s) return s;  // solutions.md §7.10 item 2
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    doc::Feature nf = *f;
    nf.name = n;
    return commit(replaceFeature(doc(), nf), "undo.renameFeature", {}, id);
}

// ---------------------------------------------------------------- §4.1а elements

Result<std::string> Session::addElement(const std::string& kind) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (kind != "part" && kind != "sheetPart" && kind != "assembly" && kind != "drawing" && kind != "specification")
        return Error("API_INVALID_ARGUMENT").with("name", std::string("kind"));
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    // Specification — of the active assembly (core-api.md §4.1а, §4.1е).
    if (kind == "specification" && !doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const std::string activeAssembly = doc().active().id;
    doc::Document nd = doc();
    doc::Element e;
    e.id = doc::allocate(nd, doc::IdKind::Element);
    e.kind = kind == "sheetPart" ? "part" : kind;
    e.name = doc::defaultElementName(nd, e.kind);
    if (kind == "sheetPart") e.sheetMetal = true;  // sheet metal part (core-api.md §4.1а, FX7-SH-002)
    if (kind == "drawing") {  // drawing — with one sheet (core-api.md §4.1г)
        doc::Sheet sh;
        sh.id = doc::allocate(nd, doc::IdKind::Sheet);
        sh.name = doc::defaultSheetName(e);
        e.sheets.push_back(std::move(sh));
    }
    if (kind == "specification") e.specAssembly = activeAssembly;
    const std::string id = e.id;
    nd.elements.push_back(std::move(e));
    nd.activeElement = id;
    if (kind == "sheetPart") {
        // The «Sheet» feature with the default parameters — in the same undo step.
        const std::string fid = doc::allocate(nd, doc::IdKind::Feature);
        auto fj = featureJsonFromDefinition({{"type", "sheetMetal"}, {"thickness", "2 mm"}, {"radius", "2 mm"}}, fid,
                                            doc::defaultFeatureName(nd.active(), doc::FeatureType::SheetMetal, ""), false);
        if (!fj) return fj.error();
        auto r = applyFeatureJson(nd, fid, *fj, 0, false);
        if (!r) return r.error();
        nd = std::move(*r);
    }
    if (auto s = commit(std::move(nd), "undo.addElement", {}, id); !s) return s.error();
    return id;
}

Status Session::sheetConvertCheck() const {
    const doc::Element& el = doc().active();
    if (el.kind != "part" || el.isSheetPart()) return badArg("kind");
    const PartResultPtr r = regenResult();
    if (!r || !r->final) return Error("API_STATE_UNAVAILABLE");
    const auto& fin = *r->final;
    // A sheet body untouched since the last sheet feature — only the kind changes (as in convertPart).
    if (fin.sheet && !fin.sheet->body.empty())
        for (const auto& b : fin.bodies)
            if (b.id == fin.sheet->body && b.shape == fin.sheet->folded) return okStatus();
    if (fin.bodies.size() != 1) return Error("SHEET_RECOGNIZE").with("detail", std::string("bodies"));
    const regen::Body& b = fin.bodies.front();
    auto m = sheet::recognize(k_, b.shape, b.bases, sheet::Params{}, std::nullopt, std::nullopt, "check");
    if (m) return okStatus();
    Error e = m.error();  // the place: FaceIdx → the stable face name (as the «Лист» feature reports it)
    if (auto it = e.params.find("face"); it != e.params.end()) {
        const int face = int(std::get<double>(it->second));
        e.params.erase(it);
        for (const auto& fe : fin.names.faces)
            if (fe.body == 0 && fe.face == face) e.with("name", fe.full);
    }
    return e;
}

Status Session::convertPart(const std::string& id, const std::string& kind) {
    if (auto s = checkNoDrag(); !s) return s;
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    doc::Document nd = doc();
    const int i = nd.elementIndex(id);
    if (i < 0 || nd.elements[size_t(i)].kind != "part") return notFound(id);
    if (kind != "sheet" && kind != "part") return badArg("kind");
    const bool toSheet = kind == "sheet";
    if (nd.elements[size_t(i)].isSheetPart() == toSheet) return badArg("kind");
    nd.activeElement = id;
    if (!toSheet) {
        // «Convert to regular» (S5-SHEET-006): always; the history and the geometry stay.
        nd.elements[size_t(i)].sheetMetal = false;
        return commit(std::move(nd), "undo.convertToPart", {}, id);
    }
    // «Convert to sheet metal»: a sheet body untouched since the last sheet feature — only the kind; otherwise a
    // «Sheet» feature with recognize at the rollback marker, checked by a separate synchronous regeneration.
    {
        regen::Regenerator rg(k_);
        auto r0 = rg.regenerate(nd, 0);
        if (!r0) return Error("API_STATE_UNAVAILABLE");
        const auto& fin = r0->final;
        bool sheetBody = false;
        if (fin && fin->sheet && !fin->sheet->body.empty())
            for (const auto& b : fin->bodies) sheetBody = sheetBody || (b.id == fin->sheet->body && b.shape == fin->sheet->folded);
        if (sheetBody) {
            nd.elements[size_t(i)].sheetMetal = true;
            return commit(std::move(nd), "undo.convertToSheet", {}, id);
        }
    }
    const int pos = insertionIndex(nd.active());
    const bool markerAtEnd = nd.active().rollback.kind == doc::Rollback::Kind::End;
    const std::string fid = doc::allocate(nd, doc::IdKind::Feature);
    auto fj = featureJsonFromDefinition({{"type", "sheetMetal"}, {"recognize", true}}, fid,
                                        doc::defaultFeatureName(nd.active(), doc::FeatureType::SheetMetal, ""), false);
    if (!fj) return fj.error();
    auto r = applyFeatureJson(nd, fid, *fj, pos, false);
    if (!r) return r.error();
    if (!markerAtEnd) r->active().rollback = {doc::Rollback::Kind::After, fid};
    r->active().sheetMetal = true;
    regen::Regenerator rg(k_);
    auto res = rg.regenerate(*r, 0);
    if (!res) return Error("API_STATE_UNAVAILABLE");
    if (auto it = res->featureStatus.find(fid); it != res->featureStatus.end())
        for (const auto& msg : it->second.messages)
            if (msg.error) return msg.e;
    return commit(std::move(*r), "undo.convertToSheet", {}, id);
}

Status Session::renameElement(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int i = nd.elementIndex(id);
    if (i < 0) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    const std::string old = nd.elements[static_cast<size_t>(i)].name;
    nd.elements[static_cast<size_t>(i)].name = n;
    if (nd.elements[static_cast<size_t>(i)].isDrawing()) nd.elements[static_cast<size_t>(i)].nameManual = true;  // FX9-DRW-003
    // Instances with the default name "<old name> <N>" — "<new name> <N>" (S2-DOC-002).
    for (auto& el : nd.elements)
        for (auto& in : el.instances)
            if (in.refElement == id && in.name.rfind(old + " <", 0) == 0 && !in.name.empty() && in.name.back() == '>')
                in.name = n + in.name.substr(old.size());
    return commit(std::move(nd), "undo.renameElement", {}, id);
}

Status Session::deleteElement(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int i = nd.elementIndex(id);
    if (i < 0) return notFound(id);
    if (nd.elements.size() == 1) return Error("API_LAST_ELEMENT").with("id", id);
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    const bool wasActive = nd.active().id == id;
    const std::string name = nd.elements[static_cast<size_t>(i)].name;
    if (nd.elements[static_cast<size_t>(i)].isAssembly()) {  // its connections go, and their owned cuts (solutions.md §7.10)
        OwnedScope scope;
        scope.assembly = id;
        scope.title = [this](const std::string& t) { return libraryText(t); };  // library.md §17.4
        const std::string self = selfPath();
        scope.owned = [id, self](const doc::Feature& f) { return regen::ownedByAssembly(f, id, true, self, self); };
        syncOwnedCuts(nd, scope);
    }
    nd.elements.erase(nd.elements.begin() + i);
    if (wasActive) nd.activeElement = nd.elements[static_cast<size_t>(std::min<int>(i, int(nd.elements.size()) - 1))].id;
    // The deleted element's name is taken from the previous document (commit looks up subject in both).
    return commit(std::move(nd), "undo.deleteElement", {}, id);
}

Status Session::moveElement(const std::string& id, int index) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int i = nd.elementIndex(id);
    if (i < 0) return notFound(id);
    if (index < 0 || index >= static_cast<int>(nd.elements.size()))
        return Error("API_INVALID_ARGUMENT").with("name", std::string("index")).with("actual", double(index));
    // The element stays on its level (FX10-TREE-001): a group member — within its group, an ungrouped element steps
    // over a whole group.
    const auto block = [&](const std::string& g) {
        int a = -1, b = -1;
        for (int k = 0; k < static_cast<int>(nd.elements.size()); ++k)
            if (nd.elements[static_cast<size_t>(k)].group == g) {
                if (a < 0) a = k;
                b = k;
            }
        return std::make_pair(a, b);
    };
    const std::string& own = nd.elements[static_cast<size_t>(i)].group;
    if (!own.empty()) {
        const auto [a, b] = block(own);
        index = std::clamp(index, a, b);
    } else if (const std::string& other = nd.elements[static_cast<size_t>(index)].group; !other.empty()) {
        const auto [a, b] = block(other);
        index = index < i ? a : b;
    }
    doc::Element e = nd.elements[static_cast<size_t>(i)];
    nd.elements.erase(nd.elements.begin() + i);
    nd.elements.insert(nd.elements.begin() + index, std::move(e));
    return commit(std::move(nd), "undo.moveElement", {}, id);
}

namespace {
// Index in elements of top-level node `node` (an ungrouped element or the first member of a group); -1 — not such a node.
int topNodeIndex(const doc::Document& d, const std::string& node) {
    for (int k = 0; k < static_cast<int>(d.elements.size()); ++k) {
        const auto& e = d.elements[static_cast<size_t>(k)];
        if ((e.id == node && e.group.empty()) || e.group == node) return k;
    }
    return -1;
}
}  // namespace

Result<std::string> Session::createElementGroup(const std::vector<std::string>& elementIds, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (elementIds.empty()) return Error("API_INVALID_ARGUMENT").with("name", std::string("elementIds"));
    doc::Document nd = doc();
    std::set<std::string> ids;
    int first = static_cast<int>(nd.elements.size());
    for (const auto& id : elementIds) {
        const int i = nd.elementIndex(id);
        if (i < 0) return notFound(id);
        ids.insert(id);
        first = std::min(first, i);
    }
    std::string n = trim(name);
    if (n.empty()) {  // default name (document.md §2.12): «Группа <N>» ("Group <N>"), the smallest free N
        std::set<std::string> taken;
        for (const auto& g : nd.elementGroups) taken.insert(g.name);
        n = doc::firstFreeName(doc::nameBase("elementGroup"), taken);
    }
    const std::string gid = doc::allocate(nd, doc::IdKind::ElementGroup);
    nd.elementGroups.push_back({gid, n});
    // The members follow the first of them in document order; the first stays in place.
    std::vector<doc::Element> members, rest;
    int at = 0;
    for (int k = 0; k < static_cast<int>(nd.elements.size()); ++k) {
        auto& e = nd.elements[static_cast<size_t>(k)];
        if (ids.count(e.id)) {
            e.group = gid;
            members.push_back(std::move(e));
        } else {
            if (k < first) ++at;
            rest.push_back(std::move(e));
        }
    }
    rest.insert(rest.begin() + at, std::make_move_iterator(members.begin()), std::make_move_iterator(members.end()));
    nd.elements = std::move(rest);
    if (auto s = commit(std::move(nd), "undo.createElementGroup"); !s) return s.error();
    return gid;
}

Status Session::renameElementGroup(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto g = std::find_if(nd.elementGroups.begin(), nd.elementGroups.end(), [&](const auto& x) { return x.id == id; });
    if (g == nd.elementGroups.end()) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    g->name = n;
    return commit(std::move(nd), "undo.renameElementGroup");
}

Status Session::dissolveElementGroup(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    if (!nd.elementGroup(id)) return notFound(id);
    for (auto& e : nd.elements)
        if (e.group == id) e.group.clear();
    std::erase_if(nd.elementGroups, [&](const auto& g) { return g.id == id; });
    return commit(std::move(nd), "undo.dissolveElementGroup");
}

Status Session::placeElement(const std::string& id, const std::string& group, const std::string& before) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int i = nd.elementIndex(id);
    if (i < 0) return notFound(id);
    if (!group.empty() && !nd.elementGroup(group)) return notFound(group);
    if (before == id) return Error("API_INVALID_ARGUMENT").with("name", std::string("before"));
    doc::Element e = std::move(nd.elements[static_cast<size_t>(i)]);
    nd.elements.erase(nd.elements.begin() + i);
    e.group = group;
    int index = static_cast<int>(nd.elements.size());
    if (!before.empty()) {
        index = group.empty() ? topNodeIndex(nd, before) : nd.elementIndex(before);
        if (index < 0 || (!group.empty() && nd.elements[static_cast<size_t>(index)].group != group))
            return Error("API_INVALID_ARGUMENT").with("name", std::string("before"));
    } else if (!group.empty()) {  // after the last member; the group of the only member — the element's own place
        index = i;
        for (int k = 0; k < static_cast<int>(nd.elements.size()); ++k)
            if (nd.elements[static_cast<size_t>(k)].group == group) index = k + 1;
    }
    nd.elements.insert(nd.elements.begin() + index, std::move(e));
    return commit(std::move(nd), "undo.moveElement", {}, id);
}

Status Session::moveElementGroup(const std::string& id, const std::string& before) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    if (!nd.elementGroup(id)) return notFound(id);
    if (before == id) return Error("API_INVALID_ARGUMENT").with("name", std::string("before"));
    std::vector<doc::Element> members, rest;
    for (auto& e : nd.elements) (e.group == id ? members : rest).push_back(std::move(e));
    nd.elements = std::move(rest);
    int index = static_cast<int>(nd.elements.size());
    if (!before.empty()) {
        index = topNodeIndex(nd, before);
        if (index < 0) return Error("API_INVALID_ARGUMENT").with("name", std::string("before"));
    }
    nd.elements.insert(nd.elements.begin() + index, std::make_move_iterator(members.begin()), std::make_move_iterator(members.end()));
    return commit(std::move(nd), "undo.moveElementGroup");
}

Status Session::setActiveElement(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (doc().elementIndex(id) < 0) return notFound(id);
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    if (doc().active().id == id) return okStatus();
    syncOwnedCutsFromPublished();  // leaving an assembly: owned cuts of its connections (solutions.md §7.10 item 5б)
    // Navigation, not an edit: no snapshot is created, the active element of the current snapshot changes (Р-025).
    undo_->setCurrentActiveElement(id);
    ++revision_;
    connectionEditing_.reset();  // FX7-ASM-004: editing ends with any change
    openSketch_.reset();
    cancelPreview();
    if (cb_.onDocumentChanged) cb_.onDocumentChanged(revision_);
    scheduleRegen();
    return okStatus();
}

Status Session::setSuppressed(const std::string& id, bool value) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::FeaturePtr f = doc().active().find(id);
    if (!f) return notFound(id);
    if (auto s = checkOwnedFeature(id, false); !s) return s;  // solutions.md §7.10 item 2
    doc::Feature nf = *f;
    nf.suppressed = value;
    doc::Document nd = replaceFeature(doc(), nf);
    if (doc().active().isAssembly() && (f->type == doc::FeatureType::Connection || f->type == doc::FeatureType::Hole))
        materializeConnections(nd, {id});  // solutions.md §7.10 item 5а; an assembly hole — assembly.md §7г.6
    return commit(std::move(nd), "undo.setSuppressed", {}, id);
}

Status Session::moveFeature(const std::string& id, int newIndex) { return moveFeatures({id}, newIndex); }

Status Session::moveFeatures(const std::vector<std::string>& ids, int newIndex) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    auto& feats = nd.active().features;
    std::set<std::string> block;
    for (const auto& id : ids) {
        if (nd.active().indexOf(id) < 0) return notFound(id);
        if (!block.insert(id).second) return Error("API_INVALID_ARGUMENT").with("name", std::string("ids"));
        if (auto s = checkOwnedFeature(id, false); !s) return s;  // solutions.md §7.10 item 2
    }
    if (ids.empty()) return Error("API_INVALID_ARGUMENT").with("name", std::string("ids"));
    // Rule В2 item 5: owned features move with their owner (they stay right before it, FX7-SEL-001); newIndex counts
    // positions without the original block, so features added to the block before it shift it.
    {
        const std::set<std::string> original = block;
        for (bool grown = true; grown;) {
            grown = false;
            for (const auto& f : feats)
                if (!block.count(f->id) && block.count(ownerOf(*f))) grown = block.insert(f->id).second || grown;
        }
        int k = 0, shift = 0;
        for (const auto& f : feats) {
            if (original.count(f->id)) continue;
            if (k++ >= newIndex) break;
            if (block.count(f->id)) ++shift;
        }
        newIndex -= shift;
    }
    std::vector<doc::FeaturePtr> moved, rest;
    for (const auto& f : feats) (block.count(f->id) ? moved : rest).push_back(f);
    if (newIndex < 0 || newIndex > static_cast<int>(rest.size()))
        return Error("API_INVALID_ARGUMENT").with("name", std::string("newIndex")).with("actual", double(newIndex));
    rest.insert(rest.begin() + newIndex, moved.begin(), moved.end());
    if (rest == feats) return okStatus();
    feats = std::move(rest);
    // Dependencies — above the dependent feature (document.md §2.6); nonexistent ones are not checked.
    for (size_t i = 0; i < feats.size(); ++i) {
        std::vector<std::string> below;
        for (const auto& dep : doc::featureDependencies(*feats[i])) {
            const int di = nd.active().indexOf(dep);
            if (di > static_cast<int>(i)) below.push_back(dep);
        }
        if (!below.empty()) return Error("API_ORDER_VIOLATION").with("id", feats[i]->id).with("ids", below);
    }
    return commit(std::move(nd), "undo.moveFeature", {}, moved.front()->id);
}

Status Session::setRollback(const doc::Rollback& r) {
    if (auto s = checkNoDrag(); !s) return s;
    if (r.kind == doc::Rollback::Kind::After && doc().active().indexOf(r.feature) < 0) return notFound(r.feature);
    doc::Document nd = doc();
    nd.active().rollback = r;
    return commit(std::move(nd), "undo.setRollback");
}

// ---------------------------------------------------------------- §4.5

Result<PreviewId> Session::previewFeature(const Json& definitionIn, const std::optional<std::string>& editing, const std::vector<Json>& aux) {
    doc::Document d = doc();
    // Owned features of the definition (core-api §4.4, FX7-SEL-001): inserted before the feature, as addFeature does.
    std::vector<std::string> auxIds;
    for (size_t i = 0; i < aux.size(); ++i) auxIds.push_back(doc::allocate(d, doc::IdKind::Feature));
    Json definition = definitionIn;
    if (!substituteAux(definition, auxIds)) return invalidDefinition("aux index", "/aux");
    if (!aux.empty()) {
        const int at = editing ? d.active().indexOf(*editing) : insertionIndex(d.active());
        if (at < 0) return notFound(*editing);
        const std::string owner = editing ? *editing : "f" + std::to_string(d.counters.feature);
        if (auto st = insertAux(d, aux, auxIds, owner, at); !st) return st.error();
        if (!editing && d.active().rollback.kind == doc::Rollback::Kind::After) d.active().rollback.feature = auxIds.back();
    }
    doc::Document work = d;
    std::string id;
    int pos = 0;
    std::string name;
    bool replace = false;
    if (editing) {
        pos = d.active().indexOf(*editing);
        if (pos < 0) return notFound(*editing);
        id = *editing;
        name = d.active().features[pos]->name;
        replace = true;
    } else {
        pos = insertionIndex(d.active());
        id = doc::allocate(work, doc::IdKind::Feature);
        if (!definition.is_object() || !definition.contains("type") || !definition["type"].is_string())
            return invalidDefinition("type required", "/type");
        name = doc::defaultFeatureName(d.active(), featureTypeFromName(definition["type"]), definitionOperation(definition));
    }
    auto fj = featureJsonFromDefinition(definition, id, name, false);
    if (!fj) return fj.error();
    if ((*fj)["type"] == "connectionCut") return invalidDefinition("type", "/type");  // solutions.md §7.10 item 2
    // Edit preview — by the updateFeature rules: the feature type does not change (§4.4).
    if (editing && (*fj)["type"] != doc::featureTypeName(d.active().features[pos]->type))
        return invalidDefinition("type cannot change", "/type");
    if (auto s = checkDefinitionRefs(d, definition, id, pos); !s) return s.error();
    auto withF = applyFeatureJson(work, id, *fj, pos, replace);
    if (!withF) return withF.error();

    const PreviewId pid = "p" + std::to_string(nextPreview_++);
    auto cancel = std::make_shared<Cancel>();
    {
        std::lock_guard lk(mu_);
        if (previewCancel_) previewCancel_->requested = true;
        previewCancel_ = cancel;
        previewId_ = pid;
        preview_.reset();
    }
    auto docWith = std::make_shared<const doc::Document>(std::move(*withF));
    const uint64_t rev = revision_;
    Job job;
    job.kind = Job::Kind::Preview;
    job.cancel = cancel;
    job.run = [this, docWith, pos, pid, rev](const Cancel* c) {
        if (cancelled(c)) return;
        // Input: the state before position pos (document regeneration with a marker before the feature; cache).
        doc::Document before = *docWith;
        auto& el = before.active();
        if (pos == 0) el.rollback = {doc::Rollback::Kind::Start, {}};
        else el.rollback = {doc::Rollback::Kind::After, el.features[pos - 1]->id};
        auto pr = regen_.regenerate(before, rev, c, false);
        if (!pr) return;
        // Input key — the chain of evaluator keys (spec/regen.md §5.1).
        uint64_t key = regen::Regenerator::initialKey();
        for (int i = 0; i < pos; ++i) {
            const doc::Feature& f = *el.features[i];
            if (f.suppressed) continue;
            key = regen::Regenerator::evaluatorKey(before, f, key, pr->variables);
        }
        const doc::Feature& f = *docWith->active().features[pos];
        auto r = regen_.evaluateFeature(*docWith, f, pos, pr->final, key, pr->variables, c);
        if (!r) return;
        auto data = std::make_shared<PreviewData>();
        data->id = pid;
        data->state = (*r)->stateOut;
        data->after = pr->after;
        PreviewResult res;
        res.revision = rev;
        res.status = (*r)->status;
        res.messages = (*r)->messages;
        if (!(*r)->tool.null() && res.status != regen::FeatureStatus::Error) {
            auto ts = std::make_shared<regen::PartState>();
            regen::Body b;
            b.id = "tool";
            b.shape = (*r)->tool;
            ts->bodies.push_back(b);
            std::vector<naming::NamedBody> nb{{b.id, b.shape, {}}};
            ts->names = naming::computeNames(k_, nb);
            data->tool = ts;
            res.hasTool = true;
        }
        {
            std::lock_guard lk(mu_);
            if (cancelled(c) || previewId_ != pid) return;
            preview_ = data;
        }
        if (cb_.onPreview) cb_.onPreview(pid, res);
    };
    runner_.enqueue(std::move(job));
    return pid;
}

void Session::cancelPreview() {
    std::lock_guard lk(mu_);
    if (previewCancel_) previewCancel_->requested = true;
    previewCancel_.reset();
    previewId_.clear();
    preview_.reset();
}

// ---------------------------------------------------------------- §5

DocumentInfo Session::document() const {
    return {std::make_shared<const doc::Document>(doc()), revision_, undo_->modified()};
}

PartResultPtr Session::regenResult() const {
    std::lock_guard lk(mu_);
    return published_;
}

Result<FeatureStatusInfo> Session::featureStatus(const std::string& id) const {
    if (doc().active().indexOf(id) < 0) return notFound(id);
    FeatureStatusInfo out;
    PartResultPtr pr = regenResult();
    if (!pr) return out;
    auto it = pr->featureStatus.find(id);
    if (it == pr->featureStatus.end()) return out;
    out.evaluated = true;
    out.status = it->second.status;
    out.messages = it->second.messages;
    return out;
}

std::map<std::string, int> Session::regenStats() const {
    std::lock_guard lk(mu_);
    return stats_;
}

std::map<std::string, double> Session::regenTimings() const {
    std::lock_guard lk(mu_);
    return timings_;
}

Result<Plane> Session::sketchFrame(const doc::Feature& f) const {
    const auto* s = f.sketchData();
    if (s->plane.frame) {
        if (auto p = regen::planeFromFrame(*s->plane.frame)) return *p;
        return Error("API_STATE_UNAVAILABLE").with("detail", std::string("plane"));
    }
    if (!s->plane.onFace() && !s->plane.onFeature()) {
        if (auto p = sketch::datumPlane(s->plane.datum)) return *p;
        return Error("API_STATE_UNAVAILABLE").with("detail", std::string("plane"));
    }
    PartResultPtr pr = regenResult();
    // Sketch on a face: the coordinate system is from the face in the state before the sketch, by the same rule
    // as the evaluator's (regen.md §4.1): name resolution, planar surface, sketch.faceFrame.
    if (pr) {
        const auto& el = doc().active();
        const int idx = el.indexOf(f.id);
        regen::StatePtr before;
        if (idx == 0) before = pr->initial;
        else if (idx > 0) {
            auto a = pr->after.find(el.features[idx - 1]->id);
            if (a != pr->after.end()) before = a->second;
        }
        if (before) {
            if (s->plane.onFeature()) {  // reference plane (regen.md §4.6) — from the state before the sketch
                auto it = before->planes.find(s->plane.feature);
                if (it != before->planes.end()) return it->second;
            } else {
                std::vector<regen::Message> warnings;
                if (auto pl = regen::sketchPlaneOnFace(s->plane.face, *before, k_, warnings)) return *pl;
            }
        }
    }
    if (pr && pr->final) {
        auto it = pr->final->sketches.find(f.id);
        if (it != pr->final->sketches.end()) return it->second.frame;
        auto a = pr->after.find(f.id);
        if (a != pr->after.end() && a->second) {
            auto jt = a->second->sketches.find(f.id);
            if (jt != a->second->sketches.end()) return jt->second.frame;
        }
    }
    return Error("API_STATE_UNAVAILABLE").with("detail", std::string("sketch frame not computed"));
}

std::optional<sketch::SketchData> Session::withProjections(const doc::Document& d, const doc::Feature& f) const {
    return withProjections(d, f, *f.sketchData());
}

std::optional<sketch::SketchData> Session::withProjections(const doc::Document& d, const doc::Feature& f,
                                                           sketch::SketchData sd) const {
    const bool needed = std::any_of(sd.entities.begin(), sd.entities.end(),
                                    [](const sketch::Entity& e) { return e.type == sketch::EntityType::Projected; });
    if (!needed) return sd;
    PartResultPtr pr = regenResult();
    const int idx = d.active().indexOf(f.id);
    regen::StatePtr before;
    if (pr && idx == 0) before = pr->initial;
    else if (pr && idx > 0) {
        auto a = pr->after.find(d.active().features[idx - 1]->id);
        if (a != pr->after.end()) before = a->second;
    }
    auto frame = sketchFrame(f);
    if (!frame) return std::nullopt;
    // The "before sketch" state is not computed yet: if all projections are sketch axes (model-independent), they
    // are resolved against an empty state (core-api.md §4.6); otherwise — as before, no solving until regeneration.
    if (!before) {
        const bool axesOnly = std::all_of(sd.entities.begin(), sd.entities.end(), [](const sketch::Entity& e) {
            return e.type != sketch::EntityType::Projected || e.source == "sketch:x" || e.source == "sketch:y";
        });
        if (!axesOnly) return std::nullopt;
        before = std::make_shared<regen::PartState>();
    }
    sd.projections = regen::resolveProjections(sd, *frame, *before, k_).projections;
    return sd;
}

Result<std::shared_ptr<const std::vector<regen::BodyTarget>>> Session::sketchBodyTargets(const std::string& sketchId) const {
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(sketchId);
    if (idx < 0) return notFound(sketchId);
    const doc::Feature& f = *d.active().features[idx];
    if (!f.sketchData()) return Error("API_NOT_SKETCH").with("id", sketchId);
    PartResultPtr pr = regenResult();
    regen::StatePtr before;
    if (pr && idx == 0) before = pr->initial;
    else if (pr && idx > 0) {
        auto a = pr->after.find(d.active().features[idx - 1]->id);
        if (a != pr->after.end()) before = a->second;
    }
    auto frame = sketchFrame(f);
    if (!frame) return frame.error();
    if (!before) return std::make_shared<const std::vector<regen::BodyTarget>>();
    std::lock_guard<std::mutex> lk(mu_);
    auto& c = bodyTargetsCache_;
    if (c.targets && c.state == before && c.frame.origin == frame->origin && c.frame.x == frame->x && c.frame.y == frame->y)
        return c.targets;
    c.state = before;
    c.frame = *frame;
    c.targets = std::make_shared<const std::vector<regen::BodyTarget>>(regen::bodyTargets(*frame, *before, k_));
    return c.targets;
}

Result<std::vector<std::optional<sketch::Projection>>> Session::projectionPreview(const std::string& sketchId,
                                                                                  const std::vector<std::string>& sources) const {
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(sketchId);
    if (idx < 0) return notFound(sketchId);
    const doc::Feature& f = *d.active().features[idx];
    if (!f.sketchData()) return Error("API_NOT_SKETCH").with("id", sketchId);
    // Temporary projected entities (IDs outside the document ID space) — projections by the rules of sketch.md §2.5.
    sketch::SketchData sd = *f.sketchData();
    for (size_t i = 0; i < sources.size(); ++i) {
        sketch::Entity e;
        e.id = "~preview" + std::to_string(i + 1);
        e.type = sketch::EntityType::Projected;
        e.source = sources[i];
        e.construction = true;
        sd.entities.push_back(e);
    }
    auto withP = withProjections(d, f, sd);
    if (!withP) return Error("API_STATE_UNAVAILABLE");
    std::vector<std::optional<sketch::Projection>> out;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto it = withP->projections.find("~preview" + std::to_string(i + 1));
        out.push_back(it != withP->projections.end() ? std::optional<sketch::Projection>(it->second) : std::nullopt);
    }
    return out;
}

Result<std::vector<std::string>> Session::sketchEdgeContour(const std::string& sketchId, const std::string& edge) const {
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(sketchId);
    if (idx < 0) return notFound(sketchId);
    const doc::Feature& f = *d.active().features[idx];
    if (!f.sketchData()) return Error("API_NOT_SKETCH").with("id", sketchId);
    PartResultPtr pr = regenResult();
    regen::StatePtr before;
    if (pr && idx == 0) before = pr->initial;
    else if (pr && idx > 0) {
        auto a = pr->after.find(d.active().features[idx - 1]->id);
        if (a != pr->after.end()) before = a->second;
    }
    if (!before) return Error("API_STATE_UNAVAILABLE");
    // Projections of the edges of the name table (sketch.md §2.6.5 «Body edge contour») as temporary projected entities.
    std::vector<std::string> names;
    for (const auto& ee : before->names.edges)
        if (!ee.full.empty() && std::find(names.begin(), names.end(), ee.full) == names.end()) names.push_back(ee.full);
    if (std::find(names.begin(), names.end(), edge) == names.end()) names.push_back(edge);
    sketch::SketchData sd = *f.sketchData();
    for (size_t i = 0; i < names.size(); ++i) {
        sketch::Entity e;
        e.id = "~edge" + std::to_string(i + 1);
        e.type = sketch::EntityType::Projected;
        e.source = names[i];
        e.construction = true;
        sd.entities.push_back(e);
    }
    auto withP = withProjections(d, f, sd);
    if (!withP) return Error("API_STATE_UNAVAILABLE");
    std::vector<std::pair<std::string, sketch::Projection>> curves;
    for (size_t i = 0; i < names.size(); ++i)
        if (const auto it = withP->projections.find("~edge" + std::to_string(i + 1)); it != withP->projections.end())
            curves.push_back({names[i], it->second});
    return sketch::edgeContour(curves, edge);
}

Result<SketchState> Session::sketchState(const std::string& featureId) const {
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(featureId);
    if (idx < 0) return notFound(featureId);
    const doc::Feature& f = *d.active().features[idx];
    if (!f.sketchData()) return Error("API_NOT_SKETCH").with("id", featureId);
    if (ctx_ && *ctx_ == featureId) {
        if (!openSketch_ || openSketch_->revision != revision_) {
            auto frame = sketchFrame(f);
            if (!frame) return frame.error();  // coordinate system not computed (face not resolved yet)
            SketchState st;
            st.revision = revision_;
            st.frame = *frame;
            const sketch::SketchData sd = withProjections(d, f).value_or(*f.sketchData());
            const auto values = regen::dimensionValues(sd, regen::documentVariables(d), regen::documentUnits(d),
                                                       "/elements/" + std::to_string(d.activeIndex()) + "/features/" + std::to_string(idx));
            st.result = sketch::solve(sd, values);
            st.projections = sd.projections;
            // Weak dimensions (sketch.md §4.4, S4-WEAK-001): for an underdefined sketch (incl. with redundant ones).
            if ((st.result.status == sketch::SketchStatus::UnderConstrained ||
                 st.result.status == sketch::SketchStatus::OverConstrainedRedundant) &&
                st.result.dof && *st.result.dof > 0) {
                sketch::WeakSet ws = sketch::weakConstraints(sd, st.result.state, values);
                st.weak = std::move(ws.items);
                st.weakDof = ws.dof;
                for (const auto& [id, v] : ws.values) st.result.reference[id] = v;
            }
            const_cast<Session*>(this)->openSketch_ = st;
        }
        if (drag_) {
            // During dragging — the state of the last drag step (§5).
            SketchState st = *openSketch_;
            st.result.state = drag_->state;
            st.result.reference = drag_->reference;
            // Weak dimension values — from the intermediate state (sketch.md §4.4 item 4, S4-WEAK-003).
            for (const auto& [id, v] : sketch::measureWeak(drag_->data, drag_->state, st.weak)) st.result.reference[id] = v;
            st.result.profiles = sketch::regions(drag_->data, drag_->state);
            return st;
        }
        return *openSketch_;
    }
    PartResultPtr pr = regenResult();
    if (!pr || !pr->final) return Error("API_STATE_UNAVAILABLE").with("detail", std::string("no regeneration result"));
    auto it = pr->final->sketches.find(featureId);
    if (it == pr->final->sketches.end())
        return Error("API_STATE_UNAVAILABLE").with("detail", std::string("sketch not evaluated"));
    return SketchState{pr->revision, it->second.frame, it->second.result, it->second.projections};
}

Result<regen::StatePtr> Session::stateOf(const StateRef& ref) const {
    std::lock_guard lk(mu_);
    auto unavailable = [](const char* d) { return Error("API_STATE_UNAVAILABLE").with("detail", std::string(d)); };
    switch (ref.kind) {
        case StateRef::Kind::Initial:
            if (!published_) return unavailable("no regeneration result");
            return published_->initial;
        case StateRef::Kind::Final:
            if (!published_) return unavailable("no regeneration result");
            return published_->final;
        case StateRef::Kind::After: {
            if (published_) {
                auto it = published_->after.find(ref.id);
                if (it != published_->after.end()) return it->second;
            }
            if (preview_) {
                auto it = preview_->after.find(ref.id);
                if (it != preview_->after.end()) return it->second;
            }
            return unavailable("feature state not available");
        }
        case StateRef::Kind::Preview:
            if (!preview_ || preview_->id != ref.id) return unavailable("preview not available");
            return preview_->state;
        case StateRef::Kind::PreviewTool:
            if (!preview_ || preview_->id != ref.id || !preview_->tool) return unavailable("preview tool not available");
            return preview_->tool;
        case StateRef::Kind::Flat: {
            if (!published_ || !published_->final) return unavailable("no regeneration result");
            if (flatCache_.first != published_->final.get())
                flatCache_ = {published_->final.get(), regen::sheetFlatState(k_, *published_->final)};
            if (!flatCache_.second) return unavailable("no flat pattern");
            return flatCache_.second;
        }
    }
    return unavailable("?");
}

Result<Session::ReferenceGeometry> Session::referenceGeometry(const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    return ReferenceGeometry{(*st)->planes, (*st)->axes};
}

std::vector<regen::AssemblyDatum> Session::nestedReferenceGeometry() const {
    const auto a = assembly();
    return a ? regen::nestedDatumsWorld(*a) : std::vector<regen::AssemblyDatum>{};
}

Result<sheet::FlatPattern> Session::flatPattern() const {
    auto st = stateOf(StateRef::final());
    if (!st) return st.error();
    const auto& sh = (*st)->sheet;
    if (!sh || sh->body.empty() || sh->flat.null()) return Error("API_STATE_UNAVAILABLE").with("detail", std::string("no flat pattern"));
    return sheet::flatPattern(k_, *sh->model, sh->flat);
}

Result<std::vector<BodyInfo>> Session::bodies(const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    std::vector<BodyInfo> out;
    for (const auto& b : (*st)->bodies) {
        BodyInfo bi;
        bi.bodyId = b.id;
        bi.bbox = k_.bbox(b.shape);
        const auto mp = k_.massProperties(b.shape);
        bi.volume = mp.volume;
        bi.area = mp.area;
        const auto ct = kernel::canonicalTopology(k_, b.shape);
        bi.faces = ct.counts.faces;
        bi.edges = ct.counts.edges;
        bi.vertices = ct.counts.vertices;
        out.push_back(bi);
    }
    return out;
}

Result<io::BodyMesh> bodyMesh(const kernel::Kernel& k, const regen::PartState& st, int bodyIndex,
                              const kernel::TessParams& params, const Cancel* cancel) {
    const regen::Body& b = st.bodies.at(bodyIndex);
    auto t = k.tessellate(b.shape, params, cancel);
    if (!t) return t.error();
    std::map<int, std::string> faceName, edgeName, vertexName;
    for (const auto& f : st.names.faces)
        if (f.body == bodyIndex) faceName[f.face] = f.full;
    for (const auto& e : st.names.edges)
        if (e.body == bodyIndex) edgeName[e.logical] = e.full;
    for (const auto& v : st.names.vertices)
        if (v.body == bodyIndex) vertexName[v.vertex] = v.full;
    auto nameOf = [](const std::map<int, std::string>& m, int i) {
        auto it = m.find(i);
        return it != m.end() ? it->second : std::string("?") + std::to_string(i);
    };
    io::BodyMesh m;
    m.bodyId = b.id;
    m.chordTol = params.chordTol;
    std::vector<const kernel::FaceMesh*> faces;
    for (const auto& fm : t->faces) faces.push_back(&fm);
    std::sort(faces.begin(), faces.end(),
              [&](auto* a, auto* c) { return nameOf(faceName, a->face) < nameOf(faceName, c->face); });
    for (const auto* fm : faces) {
        const uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
        io::FaceRange fr;
        fr.name = nameOf(faceName, fm->face);
        fr.firstTriangle = static_cast<uint32_t>(m.triangles.size() / 3);
        fr.count = static_cast<uint32_t>(fm->triangles.size() / 3);
        m.positions.insert(m.positions.end(), fm->positions.begin(), fm->positions.end());
        m.normals.insert(m.normals.end(), fm->normals.begin(), fm->normals.end());
        for (uint32_t i : fm->triangles) m.triangles.push_back(base + i);
        m.faceRanges.push_back(std::move(fr));
    }
    for (const auto& e : t->edges) m.edges.push_back({nameOf(edgeName, e.edge), e.polylines});
    std::sort(m.edges.begin(), m.edges.end(), [](const auto& a, const auto& c) { return a.name < c.name; });
    for (const auto& v : t->vertices) m.vertices.push_back({nameOf(vertexName, v.vertex), v.position});
    std::sort(m.vertices.begin(), m.vertices.end(), [](const auto& a, const auto& c) { return a.name < c.name; });
    for (int fi : t->failedFaces) m.failedFaces.push_back(nameOf(faceName, fi));
    std::sort(m.failedFaces.begin(), m.failedFaces.end());
    return m;
}

Result<io::BodyMesh> Session::tessellation(const std::string& bodyId, const StateRef& ref,
                                           const kernel::TessParams& params, double acceptChordTol) {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const auto& bodies = (*st)->bodies;
    auto it = std::find_if(bodies.begin(), bodies.end(), [&](const regen::Body& b) { return b.id == bodyId; });
    if (it == bodies.end()) return notFound(bodyId);
    if (!(params.chordTol >= kernel::KERNEL_MIN_CHORD && params.chordTol <= kernel::KERNEL_MAX_CHORD))
        return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol")).with("actual", params.chordTol);
    const TessCache::Key key{*st, bodyId, params.chordTol, params.angleTol};
    if (auto cached = tessCache_.get(key, acceptChordTol)) return *cached;
    auto m = bodyMesh(k_, **st, static_cast<int>(it - bodies.begin()), params);
    if (!m) return m.error();
    ++tessellations_;
    tessCache_.put(key, *m);
    return m;
}

Result<TopologyInfo> Session::topologyInfo(const std::string& name, const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const regen::PartState* sp = st->get();
    if (naming::hasPath(name) && sp->context) sp = sp->context.get();  // context name (assembly.md §7в)
    const regen::PartState& s = *sp;
    auto it = s.names.byName.find(name);
    if (it == s.names.byName.end()) return notFound(name);
    TopologyInfo ti;
    ti.kind = it->second.kind;
    switch (it->second.kind) {
        case naming::EntityKind::Face: {
            const auto& fe = s.names.faces.at(it->second.index);
            ti.bodyId = s.bodies.at(fe.body).id;
            ti.surface = k_.faceSurface(s.bodies.at(fe.body).shape, fe.face);
            break;
        }
        case naming::EntityKind::Edge: {
            const auto& ee = s.names.edges.at(it->second.index);
            ti.bodyId = s.bodies.at(ee.body).id;
            ti.curve = k_.edgeCurve(s.bodies.at(ee.body).shape, ee.logical);
            break;
        }
        case naming::EntityKind::Vertex: {
            const auto& ve = s.names.vertices.at(it->second.index);
            ti.bodyId = s.bodies.at(ve.body).id;
            ti.position = k_.vertexPoint(s.bodies.at(ve.body).shape, ve.vertex);
            break;
        }
    }
    return ti;
}

Result<RegionMesh> Session::regionMesh(const std::string& sketchFeatureId, const std::string& regionId,
                                       double chordTol) const {
    auto st = sketchState(sketchFeatureId);
    if (!st) return st.error();
    if (!(chordTol >= sketch::REGION_MIN_CHORD && chordTol <= sketch::REGION_MAX_CHORD))
        return Error("API_INVALID_ARGUMENT").with("name", std::string("chordTol")).with("actual", chordTol);
    const sketch::Region* r = st->result.profiles.find(regionId);
    if (!r) return notFound(regionId);
    const sketch::RegionTriangles t = sketch::triangulateRegion(*r, chordTol);
    return RegionMesh{regionId, t.vertices, t.triangles, r->interiorPoint};
}

Result<ExpressionCheck> Session::checkExpression(const std::string& text, std::optional<expr::Dim> expected) const {
    const auto units = regen::documentUnits(doc());
    const auto vars = regen::documentVariables(doc(), &links_, selfPath());
    auto q = expr::evaluateField(text, expected, vars.env(), units);
    if (!q) {
        Error e = q.error();
        // An unknown name — the existing names it may be a typo of (FX9-SK-004).
        if (e.code == "EXPR_UNKNOWN_NAME")
            if (const auto* n = e.stringParam("name")) e.with("similar", expr::similarNames(*n, vars));
        return e;
    }
    ExpressionCheck out;
    out.value = q->value;
    out.dim = q->dim;
    double f = 1;
    if (q->dim == expr::LENGTH) f = expr::lengthFactor(units.length);
    else if (q->dim == expr::ANGLE) f = expr::angleFactor(units.angle);
    out.valueInDocUnits = q->value / f;
    return out;
}

// ---------------------------------------------------------------- queries for the client (§5)

expr::DocumentUnits Session::units() const { return regen::documentUnits(doc()); }

expr::VariableTable Session::variableValues() const { return regen::documentVariables(doc(), &links_, selfPath()); }

namespace {
// Number of document expressions referencing the name (expression fields of the document schema: expr, value, depth,
// radius, distance); the own expression of variable skip is not counted.
int countReferences(const Json& j, const std::string& name, const Json* skip) {
    if (&j == skip) return 0;
    int n = 0;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string& k = it.key();
            if (it->is_string() && (k == "expr" || k == "value" || k == "depth" || k == "radius" || k == "distance")) {
                if (&*it == skip) continue;
                if (auto ast = expr::parse(it->get<std::string>()); ast && expr::references(*ast).count(name)) ++n;
            } else {
                n += countReferences(*it, name, skip);
            }
        }
    } else if (j.is_array()) {
        for (const auto& x : j) n += countReferences(x, name, skip);
    }
    return n;
}
}  // namespace

int Session::variableReferenceCount(const std::string& variableId) const {
    std::string name;
    for (const auto& v : doc().variables)
        if (v.id == variableId) name = v.name;
    if (name.empty()) return 0;
    const Json dj = doc::documentToJsonValue(doc());
    const Json* own = nullptr;
    if (dj.contains("variables"))
        for (const auto& v : dj["variables"])
            if (v.value("id", "") == variableId && v.contains("expr")) own = &v["expr"];
    return countReferences(dj, name, own);
}

Result<Json> Session::featureDefinition(const std::string& featureId) const {
    const auto f = doc().active().find(featureId);
    if (!f) return notFound(featureId);
    Json def = doc::featureToJsonValue(*f);
    for (const char* k : {"id", "name", "suppressed"}) def.erase(k);
    return def;
}

Result<sketch::TrimPlan> Session::sketchTrimPreview(const std::string& curve, const Point2& pick) const {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    const sketch::SketchData& sd = *doc().active().find(*ctx_)->sketchData();  // the same data as in sketchTrim
    const auto* e = sd.findEntity(curve);
    if (!e) return notFound(curve);
    if (e->type != sketch::EntityType::Line && e->type != sketch::EntityType::Circle && e->type != sketch::EntityType::Arc &&
        !sketch::isEllipseType(e->type) && !sketch::isSplineType(e->type))  // ellipses — S4-SKT-007, splines — S4A-SPL-003
        return invalidItems(curve, "not a curve");
    return sketch::trimPlan(sd, curve, pick);
}

std::optional<Plane> Session::datumPlane(const std::string& datumId) { return sketch::datumPlane(datumId); }

kernel::TessParams Session::stlDefaultTolerances() const {
    BBox3 bb;
    bb.setEmpty();
    if (auto infos = bodies(StateRef::final()))
        for (const auto& b : *infos) bb.extend(b.bbox);
    return io::tessParams({}, bb);
}

// Thread fit (core-api.md §5 threadFit, regen.md §4.14 item 3, FX2-FEAT-001).
Result<ThreadFit> Session::threadFit(const std::string& face, const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const regen::PartState* sp = st->get();
    if (naming::hasPath(face) && sp->context) sp = sp->context.get();  // context name (assembly.md §7в)
    const auto it = sp->names.byName.find(face);
    if (it == sp->names.byName.end()) return notFound(face);
    auto notCylinder = [&] { return Error("API_INVALID_ARGUMENT").with("name", std::string("face")).with("actual", face); };
    if (it->second.kind != naming::EntityKind::Face) return notCylinder();
    const auto& fe = sp->names.faces.at(size_t(it->second.index));
    const kernel::Shape& sh = sp->bodies.at(size_t(fe.body)).shape;
    const auto si = k_.faceSurface(sh, fe.face);
    if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) return notCylinder();
    const Vec3 u = si.dir.normalized().unitOrthogonal();
    ThreadFit out;
    out.diameter = 2 * si.radius;
    out.internal = k_.faceNormalAt(sh, fe.face, si.point + si.radius * u).dot(u) < 0;  // as in regen.md §4.14 item 3
    const double D = out.diameter;
    const auto& rows = regen::threadTables()["threads"];
    // Matching designations and the suggested one: for a tapped hole — nearest drill; otherwise — nearest nominal.
    double bestTap = 1e300, bestNom = 1e300;
    int tapIdx = -1, nomIdx = -1;
    std::vector<std::pair<double, double>> groups;  // (d, smallest deviation) in order of first appearance of d
    std::vector<int> groupRow;
    for (const auto& row : rows) {
        const double d = row["d"], P = row["pitch"];
        const auto fit = regen::threadDiameterFit(d, P, D, out.internal);
        if (fit != regen::ThreadDiameterFit::None) {
            ThreadFitOption o{row["designation"].get<std::string>(), out.internal && fit == regen::ThreadDiameterFit::Nominal,
                              regen::threadTapDrill(d, P)};
            const int idx = int(out.options.size());
            if (fit == regen::ThreadDiameterFit::TapDrill && std::abs(D - o.tapDrill) < bestTap - 1e-12) bestTap = std::abs(D - o.tapDrill), tapIdx = idx;
            if (fit == regen::ThreadDiameterFit::Nominal && std::abs(D - d) < bestNom - 1e-12) bestNom = std::abs(D - d), nomIdx = idx;
            out.options.push_back(std::move(o));
        }
        // Deviation from the row's allowed diameters (0 — fits).
        const auto r = regen::threadDiameterRanges(d, P);
        auto dist = [&](double lo, double hi) { return D < lo ? lo - D : D > hi ? D - hi : 0.0; };
        double dev = dist(r.nomMin, r.nomMax);
        if (out.internal) dev = std::min(dev, dist(r.tapMin, r.tapMax));
        size_t g = 0;
        while (g < groups.size() && std::abs(groups[g].first - d) > 1e-9) ++g;
        if (g == groups.size()) {
            groups.push_back({d, dev});
            groupRow.push_back(int(&row - &rows[0]));
        } else {
            groups[g].second = std::min(groups[g].second, dev);
        }
    }
    out.suggested = tapIdx >= 0 ? tapIdx : nomIdx;
    std::vector<size_t> order(groups.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return groups[a].second < groups[b].second - 1e-12; });
    for (size_t i = 0; i < order.size() && i < THREAD_NEAREST; ++i) {
        const auto& row = rows[size_t(groupRow[order[i]])];
        const double d = row["d"], P = row["pitch"];
        const auto r = regen::threadDiameterRanges(d, P);
        out.nearest.push_back({row["designation"].get<std::string>(), d, P, r.tapMin, r.tapMax, r.nomMin, r.nomMax});
    }
    return out;
}

const Json& Session::holeTables() { return regen::holeTables(); }
const Json& Session::threadTables() { return regen::threadTables(); }
const Json& Session::materialLibrary() { return regen::materialLibrary(); }
std::vector<std::string> Session::fonts() { return font::availableFonts(); }
std::string Session::defaultFont() { return font::defaultFont(); }
std::string Session::builtinDefaultFont() { return font::builtinDefaultFont(); }
void Session::setPreferredFont(const std::string& name) { font::setPreferredFont(name); }
std::vector<std::string> Session::defaultFontDirectories() { return font::defaultFontDirectories(); }
void Session::setFontDirectories(std::vector<std::string> dirs) { font::setFontDirectories(std::move(dirs)); }
std::string Session::defaultDrawingFont(font::DrawingStandard s) { return font::defaultDrawingFont(s); }

namespace {
bool isColor(const Json& c) {
    if (!c.is_string()) return false;
    const std::string s = c;
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}
}  // namespace

Status Session::setElementProperties(const std::string& id, const Json& props) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int i = nd.elementIndex(id);
    if (i < 0) return notFound(id);
    if (!props.is_object()) return badArg("props");
    doc::Element& el = nd.elements[size_t(i)];
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string k = it.key();
        const Json& v = it.value();
        if (k == "designation") {
            if (v.is_null()) el.designation.reset();
            else if (!v.is_string()) return badArg("designation");
            else {
                const std::string t = trim(v.get<std::string>());
                if (t.empty()) el.designation.reset();
                else el.designation = t;
            }
        } else if (k == "material") {
            if (v.is_null()) {
                el.material.reset();
                continue;
            }
            if (!v.is_object()) return badArg("material");
            for (auto f = v.begin(); f != v.end(); ++f)
                if (f.key() != "id" && f.key() != "name" && f.key() != "density" && f.key() != "youngModulus" && f.key() != "poisson" &&
                    f.key() != "yieldStrength" && f.key() != "tensileStrength" && f.key() != "color")
                    return badArg("material." + f.key());
            auto num = [&](const char* f) { return v.contains(f) && v[f].is_number(); };
            if (!v.contains("name") || !v["name"].is_string() || v["name"].get<std::string>().empty()) return badArg("material.name");
            if (!num("density") || v["density"].get<double>() <= 0) return badArg("material.density");
            if (!num("youngModulus") || v["youngModulus"].get<double>() <= 0) return badArg("material.youngModulus");
            if (!num("poisson") || v["poisson"].get<double>() < 0 || v["poisson"].get<double>() >= 0.5) return badArg("material.poisson");
            if (!num("tensileStrength") || v["tensileStrength"].get<double>() <= 0) return badArg("material.tensileStrength");
            if (!v.contains("yieldStrength") || !(v["yieldStrength"].is_null() || (v["yieldStrength"].is_number() && v["yieldStrength"].get<double>() > 0)))
                return badArg("material.yieldStrength");
            if (!v.contains("color") || !isColor(v["color"])) return badArg("material.color");
            el.material = doc::canonicalMaterial(v);
        } else if (k == "color") {
            if (v.is_null()) el.color.reset();
            else if (!isColor(v)) return badArg("color");
            else el.color = v.get<std::string>();
        } else if (k == "transparent") {  // S2-VIEW-004
            if (!v.is_boolean()) return badArg("transparent");
            el.transparent = v.get<bool>();
        } else if (k == "restRoughness") {  // roughness of the remaining surfaces (FX5-PMI-001, pmi.md §2.1а)
            if (el.kind != "part" && !el.isBody()) return badArg("restRoughness");
            if (v.is_null()) {
                el.restRoughness.reset();
                continue;
            }
            if (!v.is_object()) return badArg("restRoughness");
            for (auto f = v.begin(); f != v.end(); ++f)
                if (f.key() != "sign" && f.key() != "parameter" && f.key() != "value") return badArg("restRoughness." + f.key());
            const auto oneOf = [](const Json& x, std::initializer_list<const char*> vals) {
                return x.is_string() && std::any_of(vals.begin(), vals.end(), [&](const char* c) { return x.get<std::string>() == c; });
            };
            if (!v.contains("sign") || !oneOf(v["sign"], {"any", "removal", "noRemoval"})) return badArg("restRoughness.sign");
            if (v.contains("parameter") && !oneOf(v["parameter"], {"Ra", "Rz", "Rmax"})) return badArg("restRoughness.parameter");
            if (v.contains("value") && (!v["value"].is_number() || !(v["value"].get<double>() > 0))) return badArg("restRoughness.value");
            if (v.contains("parameter") != v.contains("value")) return badArg(v.contains("parameter") ? "restRoughness.value" : "restRoughness.parameter");
            el.restRoughness = doc::canonicalRestRoughness(v);
        } else {
            return badArg(k);
        }
    }
    return commit(std::move(nd), "undo.elementProperties", {}, id);
}

Status Session::setFaceColor(const std::string& face, const std::optional<std::string>& color) {
    if (auto s = checkNoDrag(); !s) return s;
    auto pn = naming::parse(face);
    if (!pn || pn->kind != naming::EntityKind::Face) return badArg("face");
    if (color && !isColor(*color)) return badArg("color");
    doc::Document nd = doc();
    auto& fc = nd.active().faceColors;
    std::erase_if(fc, [&](const auto& p) { return p.first == face; });
    if (color) fc.push_back({face, *color});
    std::sort(fc.begin(), fc.end());
    return commit(std::move(nd), "undo.faceColor");
}

Result<Session::MassInfo> Session::massProperties(const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    MassInfo out;
    std::vector<kernel::MassProperties> parts;
    for (const auto& b : (*st)->bodies) parts.push_back(k_.massProperties(b.shape));
    for (const auto& p : parts) out.volume += p.volume, out.area += p.area;
    if (out.volume > 0) {
        Point3 c = Point3::Zero();
        for (const auto& p : parts) c += p.volume * p.centroid;
        out.centroid = c / out.volume;
        // Tensor about the common center of mass (Steiner), units mm⁵ → kg·mm² at ρ = 1 kg/m³ (×1e-9).
        Eigen::Matrix3d I = Eigen::Matrix3d::Zero();
        for (const auto& p : parts) {
            const Vec3 d = p.centroid - out.centroid;
            I += p.inertia + p.volume * (d.squaredNorm() * Eigen::Matrix3d::Identity() - d * d.transpose());
        }
        I *= 1e-9;
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(I);
        for (int k = 0; k < 3; ++k) {
            out.principalMoments[size_t(k)] = es.eigenvalues()(k);
            out.principalAxes[size_t(k)] = es.eigenvectors().col(k).normalized();
        }
    }
    const auto& el = doc().active();
    if (el.material) {
        out.density = (*el.material)["density"].get<double>();
        out.mass = *out.density * out.volume * 1e-9;
    }
    return out;
}

Result<std::vector<Session::FaceColor>> Session::faceColors(const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    std::vector<FaceColor> out;
    for (const auto& [face, color] : doc().active().faceColors) {
        const auto rr = naming::resolve(face, naming::EntityKind::Face, (*st)->names);
        if (naming::refFate(rr, true) != naming::RefFate::Resolved) continue;
        for (const auto& e : rr.entities) {
            const auto& fe = (*st)->names.faces.at(e.index);
            out.push_back({(*st)->bodies.at(fe.body).id, fe.full, color});
        }
    }
    return out;
}

Result<std::vector<regen::ThreadEntry>> Session::threads(const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    return (*st)->threads;
}

Result<std::vector<regen::GearEntry>> Session::gears(const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    return (*st)->gears;
}

std::vector<std::string> Session::visibleSketches() const { return regen::visibleSketchIds(doc().active()); }

Result<std::vector<std::string>> Session::draftAdjacentFaces(const Json& neutral, const std::vector<std::string>& faces,
                                                             const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const naming::NameTable& names = (*st)->names;
    doc::Section sec;
    sec.plane = neutral;
    sec.offset = "0";
    auto pl = api::sectionPlane(k_, **st, doc(), sec);
    if (!pl) return pl.error();
    const Vec3 n = pl->normal.normalized();
    std::set<int> seeds;
    auto seedByName = [&](const std::string& name) {
        const auto rr = naming::resolve(name, naming::EntityKind::Face, names);
        if (!rr.error)
            for (const auto& e : rr.entities) seeds.insert(e.index);
    };
    if (neutral.contains("face") && neutral["face"].is_string()) seedByName(neutral["face"].get<std::string>());
    for (const auto& f : faces) seedByName(f);
    // Planar faces with the neutral plane as their surface (a datum plane on the bottom of a block).
    std::vector<std::optional<kernel::SurfaceInfo>> surf(names.faces.size());
    auto surface = [&](int i) -> const kernel::SurfaceInfo& {
        if (!surf[size_t(i)]) {
            const auto& fe = names.faces[size_t(i)];
            surf[size_t(i)] = k_.faceSurface((*st)->bodies.at(size_t(fe.body)).shape, fe.face);
        }
        return *surf[size_t(i)];
    };
    const double tol = 1e-6;
    for (size_t i = 0; i < names.faces.size(); ++i) {
        const auto& si = surface(int(i));
        if (si.kind == kernel::SurfaceInfo::Kind::Plane && std::abs(std::abs(si.dir.normalized().dot(n)) - 1) <= tol &&
            std::abs((si.point - pl->origin).dot(n)) <= tol)
            seeds.insert(int(i));
    }
    std::set<int> out;
    for (const auto& e : names.edges) {
        for (const auto& [a, b] : {std::pair{e.f1, e.f2}, std::pair{e.f2, e.f1}})
            if (seeds.count(a) && !seeds.count(b)) {
                const auto& si = surface(b);
                if (si.kind == kernel::SurfaceInfo::Kind::Plane && std::abs(std::abs(si.dir.normalized().dot(n)) - 1) <= tol) continue;
                out.insert(b);
            }
    }
    std::vector<std::string> res;
    for (int i : out) res.push_back(names.faces[size_t(i)].full);
    return res;
}

Result<std::vector<std::string>> Session::holePoints(const std::string& sketchFeatureId) const {
    auto st = sketchState(sketchFeatureId);
    if (!st) return st.error();
    const auto f = doc().active().find(sketchFeatureId);
    if (!f || !f->sketchData()) return Error("API_NOT_SKETCH").with("id", sketchFeatureId);
    std::vector<std::string> out;
    std::vector<Point2> taken;
    for (const auto& id : sketch::holePointCandidates(*f->sketchData())) {  // regen.md §4.13 item 1
        const auto it = st->result.state.points.find(id);
        if (it == st->result.state.points.end()) continue;
        bool dup = false;
        for (const auto& q : taken) dup = dup || (q - it->second).norm() < regen::HOLE_POINT_MERGE_TOL;
        if (dup) continue;
        taken.push_back(it->second);
        out.push_back(id);
    }
    return out;
}

Result<Session::SideMaterial> Session::extrudeSideMaterial(const std::string& sketchFeatureId, const Json& profiles, bool reverse,
                                                           const StateRef& ref) const {
    // core-api.md §5 extrudeSideMaterial.
    auto sk = sketchState(sketchFeatureId);
    if (!sk) return sk.error();
    auto st = stateOf(ref);
    if (!st) return st.error();
    const auto& bodies = (*st)->bodies;
    if (bodies.empty()) return SideMaterial::None;
    std::vector<const sketch::Region*> regions;
    if (profiles.is_string() && profiles.get<std::string>() == "all") {
        for (const auto& r : sk->result.profiles.regions)
            if (r.depth % 2 == 0) regions.push_back(&r);
    } else if (profiles.is_array()) {
        for (const auto& id : profiles)
            if (id.is_string())
                if (const sketch::Region* r = sk->result.profiles.find(id.get<std::string>())) regions.push_back(r);
    }
    std::sort(regions.begin(), regions.end(), [](const sketch::Region* a, const sketch::Region* b) { return a->regionId < b->regionId; });
    std::vector<Point2> probes;
    for (const sketch::Region* r : regions) {
        const sketch::RegionTriangles t = sketch::triangulateRegion(*r, SIDE_PROBE_CHORD);
        for (const auto& tri : t.triangles) {
            // Two levels of midpoint subdivision: 16 sub-triangles, probes are their centroids.
            std::vector<std::array<Point2, 3>> level{{t.vertices[size_t(tri[0])], t.vertices[size_t(tri[1])], t.vertices[size_t(tri[2])]}};
            for (int step = 0; step < 2; ++step) {
                std::vector<std::array<Point2, 3>> next;
                for (const auto& [a, b, c] : level) {
                    const Point2 ab = (a + b) / 2, bc = (b + c) / 2, ca = (c + a) / 2;
                    next.push_back({a, ab, ca});
                    next.push_back({ab, b, bc});
                    next.push_back({ca, bc, c});
                    next.push_back({ab, bc, ca});
                }
                level = std::move(next);
            }
            for (const auto& [a, b, c] : level) probes.push_back((a + b + c) / 3);
        }
    }
    if (probes.empty()) return SideMaterial::None;
    const size_t k = (probes.size() + SIDE_PROBE_MAX - 1) / SIDE_PROBE_MAX;
    const Vec3 d = sk->frame.normal.normalized() * (reverse ? -1.0 : 1.0);
    int hits = 0, total = 0;
    for (size_t i = 0; i < probes.size(); i += k) {
        const Point3 p = sk->frame.toWorld(probes[i]);
        bool hit = false;
        for (const auto& b : bodies) hit = hit || k_.rayHitsShape(b.shape, p, d, SIDE_PROBE_EPS);
        hits += hit ? 1 : 0;
        ++total;
    }
    return hits == 0 ? SideMaterial::None : hits == total ? SideMaterial::Full : SideMaterial::Partial;
}

const std::map<std::string, std::string>& builtinNameBases() { return doc::builtinNameBases(); }

}  // namespace construe::api
