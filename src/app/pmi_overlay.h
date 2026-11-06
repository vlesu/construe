// Display of technical requirement annotations in the 3D view (spec/app-ux.md §3.4, pmi.md §6; S3-PMI-001…003):
// symbols, frames and leaders over bodies in the screen plane; label hit-testing — for editing by double click.
#pragma once

#include "api/session.h"
#include "app/camera.h"

#include <QPainter>
#include <QRectF>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::app {

// Tree node and tooltip text (app-ux §4.2в): "Roughness Ra 1,6", "Tolerance ⌀10H7", "Parallelism 0,02 A", "Datum A".
QString annotationTitle(const api::Session::AnnotationInfo& a);
// Label of a `fit` in the 3D view of a part and an assembly (pmi.md §5.2 toleranceLabel, FX13-TOL-002): «⌀30h6(−0,013)».
QString fitLabelText(const api::Session::AnnotationInfo& a);

class AnnotationOverlay {
public:
    void setAnnotations(std::vector<api::Session::AnnotationInfo> a) { items_ = std::move(a); }
    const std::vector<api::Session::AnnotationInfo>& annotations() const { return items_; }
    void clear() { items_.clear(), rects_.clear(), drawn_.clear(); }
    // Annotations hidden by the tree eye are neither drawn nor selectable (§3.4, §4.2а; FX1-UI-009).
    void setHidden(std::set<std::string> ids) { hidden_ = std::move(ids); }
    // selected — ID of the annotation selected in the tree (highlight). visible — depth test of a world point
    // (FX12-PMI-001): an annotation whose anchor (lifted off its face along the normal) is hidden by bodies is
    // neither drawn nor hit; empty — everything is visible.
    using VisibleFn = std::function<bool(const QVector3D&)>;
    void paint(QPainter& p, const Camera& cam, const std::string& selected, const VisibleFn& visible = {});
    // Anchor point lifted off the face for the depth test: anchor + normal · max(PMI_DEPTH_LIFT_PX px, PMI_DEPTH_LIFT_MM) (FX12-PMI-001).
    static QVector3D depthProbe(const api::Session::AnnotationInfo& a, const Camera& cam);
    // ID of the annotation whose label contains the view point (per the last paint).
    std::optional<std::string> hit(const QPointF& at) const;
    // Annotation label rectangle in the last paint (tests, highlight).
    std::optional<QRectF> labelRect(const std::string& id) const;
    // Drawn anchor (leader end at the face) and label point of an annotation in the last paint (FX12-PMI-002).
    std::optional<QPointF> anchorScreen(const std::string& id) const;
    std::optional<QPointF> labelScreen(const std::string& id) const;
    // ID of the annotation whose drawn anchor is within PICK_POINT_PX of the view point (FX12-PMI-002).
    std::optional<std::string> anchorHit(const QPointF& at) const;
    // Drag preview (FX12-PMI-002): the annotation is drawn with the label point (and, if given, the anchor) at these
    // view points; reset — no preview.
    void setDragPreview(const std::string& id, const QPointF& label, std::optional<QPointF> anchor);
    void clearDragPreview() { preview_.reset(); }
    // Label world point of an annotation set by dragging: anchor + labelOffset; nullopt — no offset (default place).
    static std::optional<QVector3D> labelWorld(const api::Session::AnnotationInfo& a);

private:
    std::vector<api::Session::AnnotationInfo> items_;
    std::vector<std::pair<std::string, QRectF>> rects_;
    std::set<std::string> hidden_;
    struct Drawn {
        QPointF anchor, label;
    };
    std::vector<std::pair<std::string, Drawn>> drawn_;
    struct Preview {
        std::string id;
        QPointF label;
        std::optional<QPointF> anchor;
    };
    std::optional<Preview> preview_;
};

}  // namespace construe::app
