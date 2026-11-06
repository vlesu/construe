// FreeCAD exchange (core-api §4.7е, spec/fcstd.md): opening a .FCStd as a new document, export to .FCStd; both with
// the exchange report (S4A-UI-001, S4A-REP-001).
#include "api/session.h"

#include "api/errors.h"
#include "fcstd/export.h"

namespace construe::api {

Result<FcstdReport> Session::openFcstd(const std::string& path) {
    auto p = prepareOpenFcstd(path);
    if (!p) return p.error();
    return applyOpenFcstd(std::move(*p));
}

// Preparation (§4.7з): reading the file and importing it into a new document — the long part.
Result<FcstdPrepared> Session::prepareOpenFcstd(const std::string& path, const Cancel* cancel) const {
    if (groupDepth_ > 0) return apiError("API_GROUP_OPEN");
    if (auto s = checkNoDrag(); !s) return s.error();
    if (cancelled(cancel)) return Error("API_CANCELLED");
    auto fc = fcstd::readFcstdFile(path);
    if (!fc) return fc.error();
    if (cancelled(cancel)) return Error("API_CANCELLED");
    auto imp = fcstd::importDocument(*fc, k_);
    if (!imp) return imp.error();
    if (cancelled(cancel)) return Error("API_CANCELLED");
    return FcstdPrepared{std::move(*imp)};
}

Result<FcstdReport> Session::applyOpenFcstd(FcstdPrepared prepared) {
    if (groupDepth_ > 0) return apiError("API_GROUP_OPEN");
    if (auto s = checkNoDrag(); !s) return s.error();
    // A new document that was never saved: no path, «modified» (core-api §4.7е).
    path_.reset();
    reset(std::move(prepared.import.document));
    undo_->markUnsaved();
    return FcstdReport{std::move(prepared.import.report)};
}

Result<FcstdReport> Session::exportFcstd(const std::string& path, const std::optional<std::string>& element) {
    syncAssemblyPositions();
    const doc::Document d = doc();
    if (element && d.elementIndex(*element) < 0) return notFound(*element);
    auto r = fcstd::exportDocument(d, k_, {.element = element.value_or(std::string())});
    if (!r) return r.error();
    if (auto s = io::writeFileAtomic(path, r->bytes); !s) return s.error();
    return FcstdReport{std::move(r->report)};
}

}  // namespace construe::api
