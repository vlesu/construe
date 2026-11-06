// STL and 3MF mesh import: bodies from recognized surfaces or polyhedral (spec/core-api.md §4.7д, spec/mesh-fit.md;
// S4-MESH-001).
#include "api/session.h"

#include "io/mesh_import.h"
#include "meshfit/meshfit.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace construe::api {

namespace {

std::string fileName(const std::string& path, bool stem) {
    const auto p = std::filesystem::u8path(path);
    const auto u8 = (stem ? p.stem() : p.filename()).u8string();
    return std::string(u8.begin(), u8.end());
}

// Recognition time limit of one shell (mesh-fit.md §4.10, FX10-STL-001): a watchdog raises the local flag at the
// deadline or as soon as the caller cancels; fitting and the kernel stop at their next check. The deadline counts from
// construction, not from the watchdog start, and overdue() also covers a fit that finished after it before the
// watchdog got the processor.
class ShellDeadline {
public:
    ShellDeadline(const Cancel* outer, double seconds)
        : outer_(outer),
          end_(std::chrono::steady_clock::now() +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds))) {
        th_ = std::thread([this] {
            const auto end = end_;
            std::unique_lock<std::mutex> l(m_);
            while (!done_) {
                if (cancelled(outer_)) break;
                const auto now = std::chrono::steady_clock::now();
                if (now >= end) {
                    expired_ = true;
                    break;
                }
                // Wake at the deadline itself (a limit shorter than the cancel poll must not stretch to 50 ms).
                const auto left = std::chrono::duration_cast<std::chrono::steady_clock::duration>(end - now);
                cv_.wait_for(l, std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds(50), left));
            }
            if (!done_) flag_.requested = true;
        });
    }
    ~ShellDeadline() {
        {
            std::lock_guard<std::mutex> l(m_);
            done_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    const Cancel* flag() const { return &flag_; }
    bool expired() {
        std::lock_guard<std::mutex> l(m_);
        return expired_;
    }
    // The deadline has passed (whether or not the watchdog has seen it).
    bool overdue() const { return std::chrono::steady_clock::now() >= end_; }

private:
    const Cancel* outer_;
    const std::chrono::steady_clock::time_point end_;
    Cancel flag_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false, expired_ = false;
    std::thread th_;
};

}  // namespace

Result<MeshImportReport> Session::importMesh(const std::string& path) {
    auto p = prepareImportMesh(path);
    if (!p) return p.error();
    return applyImportMesh(std::move(*p));
}

// Preparation (§4.7з): reading, shells, surface recognition and B-Rep of every body — the long part.
Result<MeshImportPrepared> Session::prepareImportMesh(const std::string& path, const Cancel* cancel, double timeLimit) const {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    if (cancelled(cancel)) return Error("API_CANCELLED");
    auto meshes = io::readMeshFile(path);
    if (!meshes) return meshes.error();
    // Shells of all meshes of the file in order (core-api §4.7д item 2).
    struct Shell {
        std::string name;
        meshfit::Mesh mesh;
    };
    std::vector<Shell> shells;
    std::optional<Error> firstError;
    for (size_t mi = 0; mi < meshes->size(); ++mi) {
        const auto& im = (*meshes)[mi];
        meshfit::Mesh m;
        m.vertices = im.vertices;
        m.triangles = im.triangles;
        auto parts = meshfit::splitShells(m);
        if (!parts) {
            if (!firstError) firstError = parts.error();
            continue;
        }
        for (auto& p : *parts) shells.push_back({im.name, std::move(p)});
    }
    if (shells.empty()) {
        if (firstError && firstError->code == "MESHFIT_NOT_CLOSED") {
            Error e("IMPORT_MESH_NOT_CLOSED");
            e.params = firstError->params;
            return e.with("file", fileName(path, false));
        }
        return Error("IMPORT_MESH_FORMAT").with("detail", std::string("empty"));
    }
    MeshImportPrepared out;
    out.revision = revision_;
    out.source = fileName(path, false);
    const std::string stem = fileName(path, true);
    for (size_t i = 0; i < shells.size(); ++i) {
        reportProgress(cancel, int(i), int(shells.size()));
        if (cancelled(cancel)) return Error("API_CANCELLED");
        const auto& sh = shells[i];
        MeshImportPrepared::Body b;
        MeshImportReport::Body& body = b.report;
        body.triangles = int(sh.mesh.triangles.size());
        kernel::Shape shape;
        Result<meshfit::FittedSolid> fitted = Error("MESHFIT_TIMEOUT");
        {
            ShellDeadline deadline(cancel, timeLimit > 0 ? timeLimit : meshfit::MESHFIT_TIME_LIMIT);
            fitted = meshfit::buildSolid(k_, sh.mesh, {}, deadline.flag());
            if (!cancelled(cancel) && ((!fitted && deadline.expired()) || (fitted && deadline.overdue())))
                fitted = Error("MESHFIT_TIMEOUT");
        }
        if (cancelled(cancel)) return Error("API_CANCELLED");
        if (fitted) {
            shape = fitted->shape;
            body.fitted = true;
            body.meshFaces = fitted->fit.meshFaces;
        } else {
            // Surfaces not recognized - a polyhedral body from triangles (mesh-fit.md §3 item 4).
            body.reason = fitted.error().code;
            kernel::MeshSpec ms;
            ms.vertices = sh.mesh.vertices;
            for (const auto& t : sh.mesh.triangles) ms.triangles.push_back({int(t[0]), int(t[1]), int(t[2])});
            auto poly = k_.solidFromMesh(ms, cancel);
            if (!poly && cancelled(cancel)) return Error("API_CANCELLED");
            if (!poly) return Error("KERNEL_IMPORT_FAILED").with("detail", poly.error().stringParam("detail") ? *poly.error().stringParam("detail") : poly.error().code);
            shape = *poly;
        }
        body.faces = k_.counts(shape).faces;
        auto brep = k_.shapeToBrep(shape);
        if (!brep) return brep.error();
        b.brep = std::move(*brep);
        b.name = !sh.name.empty() ? sh.name : stem;
        if (shells.size() > 1 && sh.name.empty()) b.name += " " + std::to_string(i + 1);
        out.bodies.push_back(std::move(b));
    }
    reportProgress(cancel, int(shells.size()), int(shells.size()));
    if (cancelled(cancel)) return Error("API_CANCELLED");
    return out;
}

Result<MeshImportReport> Session::applyImportMesh(MeshImportPrepared prepared) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    if (prepared.revision != revision_) return Error("API_STALE");
    syncAssemblyPositions();
    doc::Document nd = doc();
    MeshImportReport rep;
    for (auto& b : prepared.bodies) {
        MeshImportReport::Body body = b.report;
        doc::Element e;
        e.id = doc::allocate(nd, doc::IdKind::Element);
        e.kind = "body";
        e.name = b.name.empty() ? doc::defaultElementName(nd, "body") : b.name;
        auto f = std::make_shared<doc::Feature>();
        f->id = doc::allocate(nd, doc::IdKind::Feature);
        f->type = doc::FeatureType::Import;
        f->name = doc::defaultFeatureName(e, doc::FeatureType::Import);
        doc::PatternData pd;
        pd.def = doc::Json{{"brep", std::move(b.brep)}, {"source", prepared.source}};
        f->data = std::move(pd);
        e.features.push_back(f);
        e.rollback = {doc::Rollback::Kind::End, {}};
        body.element = e.id;
        rep.bodies.push_back(body);
        nd.elements.push_back(std::move(e));
    }
    nd.activeElement = rep.bodies.front().element;
    if (auto s = commit(std::move(nd), "undo.importMesh", {}, rep.bodies.front().element); !s) return s.error();
    return rep;
}

}  // namespace construe::api
