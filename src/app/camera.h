// 3D view camera (spec/app-ux.md §3.2–§3.3). Orthographic projection only at stage 1
// (perspective is deferred, app-ux §13.2).
#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QVector3D>

namespace construe::app {

enum class StandardView { Front, Back, Left, Right, Top, Bottom, Iso };

struct Camera {
    QVector3D direction{0, 0, -1};  // view direction: from the camera into the scene
    QVector3D up{0, 1, 0};
    QVector3D center{0, 0, 0};
    double scale = 200;             // visible height of the view area, mm
    QSize viewport{800, 600};
    double depthRange = 10000;      // half-depth of the clipping volume, mm

    QVector3D right() const { return QVector3D::crossProduct(direction, up).normalized(); }
    double mmPerPx() const { return scale / std::max(1, viewport.height()); }

    QMatrix4x4 view() const;
    QMatrix4x4 projection() const;
    QMatrix4x4 viewProjection() const { return projection() * view(); }

    // World → screen (logical px, y down); z — depth in [0,1].
    QPointF project(const QVector3D& p, double* depth = nullptr) const;
    // Screen + depth [0,1] → world; without depth — a point on the plane through center.
    QVector3D unproject(const QPointF& s, double depth) const;
    QVector3D unprojectOnCenterPlane(const QPointF& s) const;
    // Ray from a screen point: origin and direction.
    void ray(const QPointF& s, QVector3D& origin, QVector3D& dir) const;

    void setStandard(StandardView v);
    // Look against the plane normal, with the plane's Y axis up.
    void setNormalTo(const QVector3D& normal, const QVector3D& planeY);
    void fit(const QVector3D& bboxMin, const QVector3D& bboxMax);
    // Centering on an object (app-ux §5.5а, S7F-UI-001): the view center goes to the box center, the scale is such that
    // the box takes `fraction` of the view (its larger screen side); a point-like box keeps the scale.
    void frameOn(const QVector3D& bboxMin, const QVector3D& bboxMax, double fraction);

    // Rotation from the initial camera c0 by the total offset d (px) about the pivot point.
    static Camera rotated(const Camera& c0, const QPointF& d, const QVector3D& pivot);
    void rotateScreen(double degX, double degY, double degRoll, const QVector3D& pivot);
    void panPixels(const QPointF& d);
    void zoomAt(const QPointF& s, double factor);
};

// Smooth camera transition (app-ux §3.3): "ease-in-out cubic" easing for the duration fraction t ∈ [0, 1].
double easeInOutCubic(double t);
// Camera at moment t ∈ [0, 1] of the transition a → b: orientation — shortest spherical interpolation
// of quaternions, center and scale — linear, parameter — easeInOutCubic(t); t = 1 yields exactly b.
// View area size and depth are taken from b.
Camera interpolateCamera(const Camera& a, const Camera& b, double t);

}  // namespace construe::app
