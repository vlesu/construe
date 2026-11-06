// Exception boundary of the kernel (spec/kernel-interface.md §3.0а, S4-QA-004): every entry point of the interface is
// wrapped; OCCT exceptions (Standard_Failure and its descendants), standard exceptions and anything else thrown inside
// an operation never cross the interface. Operations with Result return KERNEL_INTERNAL {name, detail}; queries throw
// KernelFailure (std::runtime_error) — the query contract (§3.8). A null Shape is rejected before the inner kernel is
// called: operations — KERNEL_INVALID_ARGUMENT {name: "shape"}, queries — std::invalid_argument (a scene of
// hiddenLineProjection / sectionCut may hold null shapes: they are skipped, §3.17).
#include "kernel_occt/occt_kernel.h"

#include <Standard_DomainError.hxx>
#include <Standard_Failure.hxx>

#include <cstdio>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <malloc.h>
#include <windows.h>
#endif

namespace construe::kernel {

namespace {

std::string occtText(const Standard_Failure& f) {
    const std::string type = f.ExceptionType();
    const char* msg = f.what();
    return msg && *msg ? type + ": " + msg : type;
}

// Text of the exception being handled (inside a catch block).
std::string currentText() {
    try {
        throw;
    } catch (const Standard_Failure& f) {
        return "OCCT " + occtText(f);
    } catch (const std::bad_alloc&) {
        return "out of memory";
    } catch (const std::exception& e) {
        return std::string("std::exception: ") + e.what();
    } catch (...) {
        return "unknown exception";
    }
}

template <class R>
R nullShape(const char* op) {
    return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("shape")).with("detail", std::string(op) + ": null shape");
}

[[noreturn]] void nullShapeQuery(const char* op) { throw std::invalid_argument(std::string("kernel.") + op + ": null shape"); }

bool anyNull(const std::vector<Shape>& v) {
    for (const auto& s : v)
        if (s.null()) return true;
    return false;
}

// Hardware faults inside OCCT (spec/kernel-interface.md §3.0а): an access violation in a fillet must become an error of the
// feature, not the end of the program. C++ catch does not see them without /EHa, which would also make catch (...) of the
// whole program swallow them; the structured handler below catches only these codes around one kernel call. The frames
// between the fault and here are abandoned without destructors: OCCT handles of that call leak.
#ifdef _MSC_VER
int isHardwareFault(unsigned long code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                   code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}
// The function holds no objects with destructors: __try does not allow them. f keeps all of them.
template <class F>
unsigned long faultOf(F& f) {
    __try {
        f();
        return 0;
    } __except (isHardwareFault(GetExceptionCode())) {
        const unsigned long code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code;
    }
}
#else
// POSIX: a signal cannot be turned into a return from an arbitrary point of OCCT safely; the fault ends the process and
// the crash report tells about it (§3.0а).
template <class F>
unsigned long faultOf(F& f) {
    f();
    return 0;
}
#endif

std::string faultText(unsigned long code) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "hardware fault 0x%08lX", code);
    return buf;
}

// Operation with Result: an exception or a hardware fault → KERNEL_INTERNAL {name, detail}.
template <class F>
auto op(const char* name, F&& f) -> decltype(f()) {
    using R = decltype(f());
    std::optional<R> result;
    auto call = [&] {
        try {
            result.emplace(f());
        } catch (...) {
            result.emplace(Error("KERNEL_INTERNAL").with("name", std::string(name)).with("detail", currentText()));
        }
    };
    if (const unsigned long code = faultOf(call)) return Error("KERNEL_INTERNAL").with("name", std::string(name)).with("detail", faultText(code));
    return std::move(*result);
}

// Query (value): a standard exception passes as is (contract §3.8, e.g. index out of range); anything else →
// KernelFailure with the operation name and the OCCT exception type and message. Standard_Failure derives from
// std::exception, so it is caught first.
template <class F>
auto query(const char* name, F&& f) -> decltype(f()) {
    using R = decltype(f());
    std::exception_ptr failure;
    std::optional<std::conditional_t<std::is_void_v<R>, char, R>> result;
    auto call = [&] {
        try {
            if constexpr (std::is_void_v<R>) f();
            else result.emplace(f());
        } catch (...) {
            failure = std::current_exception();  // thrown again outside __try
        }
    };
    if (const unsigned long code = faultOf(call)) throw KernelFailure(std::string("kernel.") + name + ": " + faultText(code));
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const KernelFailure&) {
            throw;
        } catch (const Standard_Failure&) {
            throw KernelFailure(std::string("kernel.") + name + ": " + currentText());
        } catch (const std::exception&) {
            throw;
        } catch (...) {
            throw KernelFailure(std::string("kernel.") + name + ": " + currentText());
        }
    }
    if constexpr (!std::is_void_v<R>) return std::move(*result);
}

class GuardedKernel final : public Kernel {
public:
    explicit GuardedKernel(std::unique_ptr<Kernel> inner) : k_(std::move(inner)) {}

    Result<OpResult> extrude(const Profile& p, const Extent& e, bool r) const override {
        return op("extrude", [&] { return k_->extrude(p, e, r); });
    }
    Result<OpResult> revolve(const Profile& p, const Axis& a, const RevolveExtent& e) const override {
        return op("revolve", [&] { return k_->revolve(p, a, e); });
    }
    Result<OpResult> boolean(BoolKind kind, const Shape& t, const Shape& tool, const Cancel* c) const override {
        if (t.null() || tool.null()) return nullShape<Result<OpResult>>("boolean");
        return op("boolean", [&] { return k_->boolean(kind, t, tool, c); });
    }
    Result<OpResult> transform(const Shape& s, const Motion& m) const override {
        if (s.null()) return nullShape<Result<OpResult>>("transform");
        return op("transform", [&] { return k_->transform(s, m); });
    }
    Result<OpResult> shell(const Shape& b, const std::vector<int>& f, double t, bool o, const Cancel* c) const override {
        if (b.null()) return nullShape<Result<OpResult>>("shell");
        return op("shell", [&] { return k_->shell(b, f, t, o, c); });
    }
    Result<OpResult> draft(const Shape& b, const std::vector<int>& f, const Point3& p, const Vec3& d, double a, const Cancel* c) const override {
        if (b.null()) return nullShape<Result<OpResult>>("draft");
        return op("draft", [&] { return k_->draft(b, f, p, d, a, c); });
    }
    Result<OpResult> loft(const std::vector<Profile>& s, const Cancel* c) const override {
        return op("loft", [&] { return k_->loft(s, c); });
    }
    Result<OpResult> helicalSweep(const Profile& p, const Axis& a, double pitch, double turns, const Cancel* c) const override {
        return op("helicalSweep", [&] { return k_->helicalSweep(p, a, pitch, turns, c); });
    }
    Result<OpResult> pipeSweep(const Profile& p, const std::vector<PathSegment>& path, const Cancel* c) const override {
        return op("pipeSweep", [&] { return k_->pipeSweep(p, path, c); });
    }
    Result<OpResult> fillet(const Shape& b, const std::vector<EdgeRef>& e, double r, const Cancel* c) const override {
        if (b.null()) return nullShape<Result<OpResult>>("fillet");
        return op("fillet", [&] { return k_->fillet(b, e, r, c); });
    }
    Result<OpResult> chamfer(const Shape& b, const std::vector<EdgeRef>& e, double d, const Cancel* c) const override {
        if (b.null()) return nullShape<Result<OpResult>>("chamfer");
        return op("chamfer", [&] { return k_->chamfer(b, e, d, c); });
    }
    Result<Tessellation> tessellate(const Shape& b, const TessParams& p, const Cancel* c) const override {
        if (b.null()) return nullShape<Result<Tessellation>>("tessellate");
        return op("tessellate", [&] { return k_->tessellate(b, p, c); });
    }

    Counts counts(const Shape& s) const override {
        if (s.null()) nullShapeQuery("counts");
        return query("counts", [&] { return k_->counts(s); });
    }
    std::vector<int> solidFaces(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("solidFaces");
        return query("solidFaces", [&] { return k_->solidFaces(s, i); });
    }
    std::vector<int> faceEdges(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("faceEdges");
        return query("faceEdges", [&] { return k_->faceEdges(s, i); });
    }
    std::pair<int, int> edgeFaces(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("edgeFaces");
        return query("edgeFaces", [&] { return k_->edgeFaces(s, i); });
    }
    std::pair<int, int> edgeVertices(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("edgeVertices");
        return query("edgeVertices", [&] { return k_->edgeVertices(s, i); });
    }
    std::vector<int> vertexFaces(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("vertexFaces");
        return query("vertexFaces", [&] { return k_->vertexFaces(s, i); });
    }
    std::vector<int> vertexEdges(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("vertexEdges");
        return query("vertexEdges", [&] { return k_->vertexEdges(s, i); });
    }

    MassProperties massProperties(const Shape& s) const override {
        if (s.null()) nullShapeQuery("massProperties");
        return query("massProperties", [&] { return k_->massProperties(s); });
    }
    BBox3 bbox(const Shape& s) const override {
        if (s.null()) nullShapeQuery("bbox");
        return query("bbox", [&] { return k_->bbox(s); });
    }
    double faceArea(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("faceArea");
        return query("faceArea", [&] { return k_->faceArea(s, i); });
    }
    Point3 faceCentroid(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("faceCentroid");
        return query("faceCentroid", [&] { return k_->faceCentroid(s, i); });
    }
    double edgeLength(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("edgeLength");
        return query("edgeLength", [&] { return k_->edgeLength(s, i); });
    }
    Point3 edgeCentroid(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("edgeCentroid");
        return query("edgeCentroid", [&] { return k_->edgeCentroid(s, i); });
    }
    Point3 edgeMidpoint(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("edgeMidpoint");
        return query("edgeMidpoint", [&] { return k_->edgeMidpoint(s, i); });
    }
    Point3 vertexPoint(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("vertexPoint");
        return query("vertexPoint", [&] { return k_->vertexPoint(s, i); });
    }
    Vec3 faceNormalAt(const Shape& s, int i, const Point3& p) const override {
        if (s.null()) nullShapeQuery("faceNormalAt");
        return query("faceNormalAt", [&] { return k_->faceNormalAt(s, i, p); });
    }
    double distanceToFace(const Shape& s, int i, const Point3& p) const override {
        if (s.null()) nullShapeQuery("distanceToFace");
        return query("distanceToFace", [&] { return k_->distanceToFace(s, i, p); });
    }
    Point3 closestPointOnFace(const Shape& s, int i, const Point3& p) const override {
        if (s.null()) nullShapeQuery("closestPointOnFace");
        return query("closestPointOnFace", [&] { return k_->closestPointOnFace(s, i, p); });
    }
    double distanceToEdge(const Shape& s, int i, const Point3& p) const override {
        if (s.null()) nullShapeQuery("distanceToEdge");
        return query("distanceToEdge", [&] { return k_->distanceToEdge(s, i, p); });
    }
    bool rayHitsShape(const Shape& s, const Point3& p, const Vec3& d, double tMin) const override {
        return query("rayHitsShape", [&] { return k_->rayHitsShape(s, p, d, tMin); });
    }
    SurfaceInfo faceSurface(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("faceSurface");
        return query("faceSurface", [&] { return k_->faceSurface(s, i); });
    }
    CurveInfo edgeCurve(const Shape& s, int i) const override {
        if (s.null()) nullShapeQuery("edgeCurve");
        return query("edgeCurve", [&] { return k_->edgeCurve(s, i); });
    }
    Result<Curve2> projectEdge(const Shape& s, int i, const Plane& p) const override {
        if (s.null()) return nullShape<Result<Curve2>>("projectEdge");
        return op("projectEdge", [&] { return k_->projectEdge(s, i, p); });
    }
    Result<Point3> extremePoint(const Shape& s, const CurveSet& q, const Vec3& d) const override {
        if (s.null()) return nullShape<Result<Point3>>("extremePoint");
        return op("extremePoint", [&] { return k_->extremePoint(s, q, d); });
    }
    Result<std::vector<std::vector<Point3>>> curvePoints(const Shape& s, const CurveSet& q, double deflection) const override {
        if (s.null()) return nullShape<Result<std::vector<std::vector<Point3>>>>("curvePoints");
        return op("curvePoints", [&] { return k_->curvePoints(s, q, deflection); });
    }
    double distanceToShape(const Shape& s, const Point3& p) const override {
        return query("distanceToShape", [&] { return k_->distanceToShape(s, p); });
    }

    Result<HlrProjection> hiddenLineProjection(const std::vector<PlacedShape>& items, const ViewDirection& v, const HlrParams& p,
                                               const Cancel* c) const override {
        return op("hiddenLineProjection", [&] { return k_->hiddenLineProjection(items, v, p, c); });
    }
    Result<SectionCut> sectionCut(const std::vector<PlacedShape>& items, const ViewDirection& v, const Point3& o, const HlrParams& p,
                                  const Cancel* c) const override {
        return op("sectionCut", [&] { return k_->sectionCut(items, v, o, p, c); });
    }

    Result<OpResult> removeFaces(const Shape& b, const std::vector<int>& f, const Cancel* c) const override {
        if (b.null()) return nullShape<Result<OpResult>>("removeFaces");
        return op("removeFaces", [&] { return k_->removeFaces(b, f, c); });
    }
    Result<std::vector<Curve2>> planeSection(const Shape& s, const Plane& p, bool splines) const override {
        return op("planeSection", [&] { return k_->planeSection(s, p, splines); });
    }
    Result<Shape> solidFromFaces(const SolidSpec& s, const Cancel* c) const override {
        return op("solidFromFaces", [&] { return k_->solidFromFaces(s, c); });
    }
    Result<Shape> solidFromMesh(const MeshSpec& m, const Cancel* c) const override {
        return op("solidFromMesh", [&] { return k_->solidFromMesh(m, c); });
    }

    ValidityReport checkValidity(const Shape& s) const override {
        if (s.null()) nullShapeQuery("checkValidity");
        return query("checkValidity", [&] { return k_->checkValidity(s); });
    }
    Result<Shape> unifyFaces(const Shape& s) const override {
        if (s.null()) return nullShape<Result<Shape>>("unifyFaces");
        return op("unifyFaces", [&] { return k_->unifyFaces(s); });
    }
    Result<HealResult> healShape(const Shape& s, double t) const override {
        if (s.null()) return nullShape<Result<HealResult>>("healShape");
        return op("healShape", [&] { return k_->healShape(s, t); });
    }
    OpResult combine(const std::vector<Shape>& shapes) const override {
        if (anyNull(shapes)) nullShapeQuery("combine");
        return query("combine", [&] { return k_->combine(shapes); });
    }
    std::vector<std::pair<Shape, std::vector<int>>> components(const Shape& s) const override {
        if (s.null()) nullShapeQuery("components");
        return query("components", [&] { return k_->components(s); });
    }
    KernelInfo info() const override { return query("info", [&] { return k_->info(); }); }

    Result<std::string> shapeToBrep(const Shape& s) const override {
        if (s.null()) return nullShape<Result<std::string>>("shapeToBrep");
        return op("shapeToBrep", [&] { return k_->shapeToBrep(s); });
    }
    Result<Shape> shapeFromBrep(const std::string& text) const override {
        return op("shapeFromBrep", [&] { return k_->shapeFromBrep(text); });
    }
    Result<std::string> shapesToBlob(const std::vector<Shape>& shapes) const override {
        if (anyNull(shapes)) return nullShape<Result<std::string>>("shapesToBlob");
        return op("shapesToBlob", [&] { return k_->shapesToBlob(shapes); });
    }
    Result<std::vector<Shape>> shapesFromBlob(const std::string& blob) const override {
        return op("shapesFromBlob", [&] { return k_->shapesFromBlob(blob); });
    }
    Status writeStep(const std::string& path, const StepModel& model) const override {
        for (const auto& p : model.products)
            if (p.shape && p.shape->null()) return nullShape<Status>("writeStep");
        return op("writeStep", [&] { return k_->writeStep(path, model); });
    }
    Result<StepModel> readStep(const std::string& path) const override {
        return op("readStep", [&] { return k_->readStep(path); });
    }

private:
    std::unique_ptr<Kernel> k_;
};

}  // namespace

void throwOcctFailure(const std::string& message) { throw Standard_DomainError(message.c_str()); }

std::unique_ptr<Kernel> guardKernel(std::unique_ptr<Kernel> inner) { return std::make_unique<GuardedKernel>(std::move(inner)); }

}  // namespace construe::kernel
