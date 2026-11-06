// 3D view camera (spec/app-ux.md §3.2–§3.3).
#include "app/camera.h"

#include "app/theme.h"

#include <algorithm>
#include <cmath>

namespace construe::app {

QMatrix4x4 Camera::view() const {
    QMatrix4x4 m;
    m.lookAt(center - direction * float(depthRange * 0.5), center, up);
    return m;
}

QMatrix4x4 Camera::projection() const {
    QMatrix4x4 m;
    const double h = scale / 2;
    const double w = h * viewport.width() / std::max(1, viewport.height());
    m.ortho(float(-w), float(w), float(-h), float(h), 0.0f, float(depthRange));
    return m;
}

QPointF Camera::project(const QVector3D& p, double* depth) const {
    const QVector4D c = viewProjection() * QVector4D(p, 1);
    const double x = c.x() / c.w(), y = c.y() / c.w(), z = c.z() / c.w();
    if (depth) *depth = z * 0.5 + 0.5;
    return {(x * 0.5 + 0.5) * viewport.width(), (1 - (y * 0.5 + 0.5)) * viewport.height()};
}

QVector3D Camera::unproject(const QPointF& s, double depth) const {
    const double x = s.x() / viewport.width() * 2 - 1;
    const double y = 1 - s.y() / viewport.height() * 2;
    const QVector4D w = viewProjection().inverted() * QVector4D(float(x), float(y), float(depth * 2 - 1), 1);
    return w.toVector3D() / w.w();
}

QVector3D Camera::unprojectOnCenterPlane(const QPointF& s) const {
    const QPointF cs = project(center);
    const QPointF d = s - cs;
    const double k = mmPerPx();
    return center + right() * float(d.x() * k) - up * float(d.y() * k);
}

void Camera::ray(const QPointF& s, QVector3D& origin, QVector3D& dir) const {
    origin = unprojectOnCenterPlane(s) - direction * float(depthRange * 0.5);
    dir = direction;
}

void Camera::setStandard(StandardView v) {
    switch (v) {
        case StandardView::Front: direction = {0, 0, -1}; up = {0, 1, 0}; break;
        case StandardView::Back: direction = {0, 0, 1}; up = {0, 1, 0}; break;
        case StandardView::Left: direction = {1, 0, 0}; up = {0, 1, 0}; break;
        case StandardView::Right: direction = {-1, 0, 0}; up = {0, 1, 0}; break;
        case StandardView::Top: direction = {0, -1, 0}; up = {0, 0, -1}; break;
        case StandardView::Bottom: direction = {0, 1, 0}; up = {0, 0, 1}; break;
        case StandardView::Iso: {
            direction = QVector3D(-1, -1, -1).normalized();
            QVector3D y(0, 1, 0);
            up = (y - direction * QVector3D::dotProduct(y, direction)).normalized();
            break;
        }
    }
}

void Camera::setNormalTo(const QVector3D& normal, const QVector3D& planeY) {
    direction = -normal.normalized();
    up = planeY.normalized();
}

void Camera::fit(const QVector3D& mn, const QVector3D& mx) {
    center = (mn + mx) * 0.5f;
    // Bounding box in screen axes.
    const QVector3D r = right();
    double w = 0, h = 0;
    for (int i = 0; i < 8; ++i) {
        const QVector3D p((i & 1) ? mx.x() : mn.x(), (i & 2) ? mx.y() : mn.y(), (i & 4) ? mx.z() : mn.z());
        const QVector3D d = p - center;
        w = std::max(w, 2.0 * std::abs(QVector3D::dotProduct(d, r)));
        h = std::max(h, 2.0 * std::abs(QVector3D::dotProduct(d, up)));
    }
    const double aspect = double(viewport.width()) / std::max(1, viewport.height());
    double need = std::max(h, w / aspect) * (1 + 2 * FIT_MARGIN_FRACTION);
    scale = std::clamp(need, ZOOM_MIN_HEIGHT_MM, ZOOM_MAX_HEIGHT_MM);
}

void Camera::frameOn(const QVector3D& mn, const QVector3D& mx, double fraction) {
    center = (mn + mx) * 0.5f;
    const QVector3D r = right();
    double w = 0, h = 0;
    for (int i = 0; i < 8; ++i) {
        const QVector3D p((i & 1) ? mx.x() : mn.x(), (i & 2) ? mx.y() : mn.y(), (i & 4) ? mx.z() : mn.z());
        const QVector3D d = p - center;
        w = std::max(w, 2.0 * std::abs(QVector3D::dotProduct(d, r)));
        h = std::max(h, 2.0 * std::abs(QVector3D::dotProduct(d, up)));
    }
    const double aspect = double(viewport.width()) / std::max(1, viewport.height());
    const double need = std::max(h, w / aspect) / std::max(fraction, 1e-3);
    if (need > ZOOM_MIN_HEIGHT_MM) scale = std::clamp(need, ZOOM_MIN_HEIGHT_MM, ZOOM_MAX_HEIGHT_MM);
}

double easeInOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(2 - 2 * t, 3) / 2;
}

namespace {
// Orientation: screen axes to the right, up, toward the viewer.
QQuaternion orientation(const Camera& c) { return QQuaternion::fromAxes(c.right(), c.up, -c.direction).normalized(); }
}  // namespace

Camera interpolateCamera(const Camera& a, const Camera& b, double t) {
    if (t >= 1) return b;
    const double s = easeInOutCubic(t);
    Camera c = b;
    const QQuaternion q = QQuaternion::slerp(orientation(a), orientation(b), float(s)).normalized();
    c.direction = -q.rotatedVector(QVector3D(0, 0, 1)).normalized();
    c.up = q.rotatedVector(QVector3D(0, 1, 0)).normalized();
    c.center = a.center + (b.center - a.center) * float(s);
    c.scale = a.scale + (b.scale - a.scale) * s;
    return c;
}

Camera Camera::rotated(const Camera& c0, const QPointF& d, const QVector3D& pivot) {
    const double len = std::hypot(d.x(), d.y());
    if (len == 0) return c0;
    const QVector3D a = (c0.up * float(d.x()) + c0.right() * float(d.y())).normalized();
    const float theta = float(len * ROTATE_DEG_PER_PX);
    // The model rotates by θ about a ⇔ the camera by −θ.
    const QQuaternion q = QQuaternion::fromAxisAndAngle(a, -theta);
    Camera c = c0;
    c.direction = q.rotatedVector(c0.direction).normalized();
    c.up = q.rotatedVector(c0.up).normalized();
    c.center = pivot + q.rotatedVector(c0.center - pivot);
    return c;
}

void Camera::rotateScreen(double degX, double degY, double degRoll, const QVector3D& pivot) {
    QQuaternion q = QQuaternion::fromAxisAndAngle(up, float(-degX)) * QQuaternion::fromAxisAndAngle(right(), float(-degY)) *
                    QQuaternion::fromAxisAndAngle(direction, float(degRoll));
    direction = q.rotatedVector(direction).normalized();
    up = q.rotatedVector(up).normalized();
    center = pivot + q.rotatedVector(center - pivot);
}

void Camera::panPixels(const QPointF& d) {
    const double k = mmPerPx();
    center -= right() * float(d.x() * k) - up * float(d.y() * k);
}

void Camera::zoomAt(const QPointF& s, double factor) {
    const double newScale = std::clamp(scale / factor, ZOOM_MIN_HEIGHT_MM, ZOOM_MAX_HEIGHT_MM);
    const QVector3D p = unprojectOnCenterPlane(s);
    const double f = newScale / scale;
    center = p + (center - p) * float(f);
    scale = newScale;
}

}  // namespace construe::app
