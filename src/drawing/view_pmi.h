// Technical requirement annotations on a drawing (spec/drawing.md §6.18, §6а.3): resolving the annotation face in the
// model state and the tolerance from the model in a dimension label. Internal to the drawing module.
#pragma once
#include "drawing/sheet.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::drawing::detail {

// Annotation face resolved in the state (§6.18; core-api.md §5 AnnotationInfo): EXACT/REMAPPED — the entity,
// SPLIT — the first part; otherwise nullopt.
struct AnnotationFace {
    int entry = 0;  // index in the state face name table
    Point3 anchor = Point3::Zero();
    Vec3 normal = Vec3::UnitZ();
    kernel::SurfaceInfo surface;
};
std::optional<AnnotationFace> resolveAnnotationFace(const kernel::Kernel& k, const regen::PartState& s, const std::string& faceName);

// Element of this document with annotations for the main view (part, body or assembly — FX12-PMI-003; empty ref.file);
// none — nullptr.
const doc::Element* annotatedModel(const doc::Document& d, const doc::DrawingView& base);

// Tolerance from the model (§6а.3): fit designation and the nominal from which the zone deviations are computed.
struct ModelFit {
    const doc::Json* fit = nullptr;
    double nominal = 0;
};
// First fit designation on a cylindrical face of radius radius resolved into one of the faces faces (face name table
// indices); nominal — face diameter. None — nullopt.
std::optional<ModelFit> faceFit(const doc::Element& el, const kernel::Kernel& k, const regen::PartState& s, const std::vector<int>& faces,
                                double radius);
// Tolerance of the hole diameter (FX12-PMI-007) whose wall has the base faceBase («F.side(s1/…)»); none — nullptr.
const doc::Json* holeWallTolerance(const doc::Element& el, const std::string& faceBase);
// Tolerance text after the nominal by the display choice mode (§6а.3, FX3-DRW-001): none — as in the model (pmi.md §5.2), hide — empty,
// field — zone ("H7"), fieldDeviations — zone with deviations ("H7(+0,015)"), deviations — deviations ("␣+0,015"); for
// explicit deviations there is no zone — deviations are shown (except hide).
std::string toleranceText(const doc::Json& fit, double nominal, const std::optional<std::string>& mode);

}  // namespace construe::drawing::detail
