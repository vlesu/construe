// 3D view (docs/dev/client.md §4, spec/app-ux.md §3).
#include "app/viewport.h"

#include <QHelpEvent>
#include <QToolTip>

#include "api/session.h"
#include "app/gl_setup.h"
#include "app/messages.h"
#include "base/log.h"

#include <QApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include "app/keys.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>

namespace construe::app {

namespace {

QVector3D q(const Vec3& v) { return QVector3D(float(v.x()), float(v.y()), float(v.z())); }


double segDist(const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF ab = b - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t = l2 > 0 ? std::clamp(((p - a).x() * ab.x() + (p - a).y() * ab.y()) / l2, 0.0, 1.0) : 0.0;
    const QPointF c = a + ab * t;
    return std::hypot(p.x() - c.x(), p.y() - c.y());
}

}  // namespace

Viewport::Viewport(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(200, 200);
    camera_.setStandard(StandardView::Iso);
    setReferenceGeometry({}, {}, false);  // origin planes (§3.4)
}

Viewport::~Viewport() {
    // All GL resources are released while the widget's context is current.
    makeCurrent();
    renderer_.release();
    doneCurrent();
}

void Viewport::setCamera(const Camera& c) {
    stopAnimation();  // an instant set cancels a running transition
    const QSize vp = camera_.viewport;
    camera_ = c;
    camera_.viewport = vp;
    dirty_ = true;
    update();
    emit cameraChanged();
}

void Viewport::animateTo(const Camera& target) {
    if (animMs_ <= 0) {
        stopAnimation();
        setCamera(target);
        return;
    }
    animFrom_ = camera_;  // a new transition starts from the current (possibly intermediate) position
    animTo_ = target;
    animTo_.viewport = camera_.viewport;
    if (!animTimer_) {
        animTimer_ = new QTimer(this);
        animTimer_->setInterval(16);
        connect(animTimer_, &QTimer::timeout, this, &Viewport::animationStep);
    }
    animClock_.start();
    animTimer_->start();
}

void Viewport::animationStep() {
    const double t = double(animClock_.elapsed()) / animMs_;
    if (t >= 1) animTimer_->stop();
    Camera c = interpolateCamera(animFrom_, animTo_, t);
    c.viewport = camera_.viewport;
    camera_ = c;
    dirty_ = true;
    update();
    emit cameraChanged();
}

void Viewport::stopAnimation() {
    if (animTimer_) animTimer_->stop();
}

Camera Viewport::targetCamera() const {
    if (!isAnimating()) return camera_;
    Camera c = animTo_;
    c.viewport = camera_.viewport;
    return c;
}

void Viewport::setBodies(std::vector<DrawBody> bodies) {
    bodies_ = std::move(bodies);
    bodiesDirty_ = true;
    ++sceneGen_;
    dirty_ = true;
    update();
    emit bodiesSet();
}

void Viewport::setPlanesVisible(bool v) {
    if (planesVisible_ == v) return;
    planesVisible_ = v;
    ++sceneGen_;
    dirty_ = true;
    update();
}

void Viewport::setTreeHover(const std::set<PickTarget>& targets, bool emphasis) {
    if (treeHover_ == targets && treeHoverEmphasis_ == emphasis) return;
    treeHover_ = targets;
    treeHoverEmphasis_ = emphasis;
    update();
}

void Viewport::setShownPlanes(const std::set<std::string>& ids) {
    if (shownPlanes_ == ids) return;
    shownPlanes_ = ids;
    ++sceneGen_;
    dirty_ = true;
    update();
}

void Viewport::setReferenceGeometry(std::vector<DrawPlane> planes, std::vector<DrawAxis> axes, bool allShown) {
    std::vector<DrawPlane> all;
    for (const auto& pd : DATUM_PLANES)
        all.push_back({pd.id, QString::fromUtf8(txt(pd.label)), *api::Session::datumPlane(pd.id), QVector3D(0, 0, 0)});
    for (auto& p : planes) all.push_back(std::move(p));
    planes_ = std::move(all);
    refAxes_ = std::move(axes);
    allPlanesShown_ = allShown;
    ++sceneGen_;
    dirty_ = true;
    update();
}

void Viewport::setWelds(std::vector<DrawWeld> welds, bool pickable) {
    welds_ = std::move(welds);
    weldsPickable_ = pickable;
    dirty_ = true;
    update();
}

void Viewport::setNestedReference(DrawNestedReference n) {
    nested_ = std::move(n);
    dirty_ = true;
    update();
}

void Viewport::setNestedPickable(bool on) {
    if (nestedPickable_ == on) return;
    nestedPickable_ = on;
    update();
}

namespace {
// Pick target of a nested plane / axis ID "<node path>/<feature ID>" (FX9-ASM-014).
PickTarget nestedTarget(PickTarget::Kind kind, const std::string& id) {
    const size_t sl = id.rfind('/');
    return sl == std::string::npos ? PickTarget{kind, {}, id} : PickTarget{kind, id.substr(0, sl), id.substr(sl + 1)};
}
}  // namespace

const DrawPlane* Viewport::nestedPlaneOf(const PickTarget& t) const {
    if (t.kind != PickTarget::Kind::Plane || t.body.empty()) return nullptr;
    for (const auto& pd : nested_.planes)
        if (nestedTarget(PickTarget::Kind::Plane, pd.id) == t) return &pd;
    return nullptr;
}

const DrawAxis* Viewport::nestedAxisOf(const PickTarget& t) const {
    if (t.kind != PickTarget::Kind::Axis || t.body.empty()) return nullptr;
    for (const auto& a : nested_.axes)
        if (nestedTarget(PickTarget::Kind::Axis, a.id) == t) return &a;
    return nullptr;
}

QPolygonF Viewport::nestedPlaneQuad(const DrawPlane& pd) const {
    auto q = [](const Vec3& v) { return QVector3D(float(v.x()), float(v.y()), float(v.z())); };
    const QVector3D x = q(pd.frame.x) * float(planeHalf_), y = q(pd.frame.y) * float(planeHalf_);
    return QPolygonF{camera_.project(pd.center - x - y), camera_.project(pd.center + x - y), camera_.project(pd.center + x + y),
                     camera_.project(pd.center - x + y)};
}

bool Viewport::nestedPlaneShown(const std::string& id) const {
    if (!planesVisible_) return false;
    return allPlanesShown_ || bodies_.empty() || shownPlanes_.count(id) > 0;
}

// Nested subassemblies (app-ux §3.4, FX9-ASM-008): planes — border and label, axes — dash-dot, sketches — sketch lines.
void Viewport::drawNestedReference(QPainter& p) {
    // Hovered / selected — in the selection colors (mate mode, FX9-ASM-014).
    auto marked = [&](const PickTarget& t) -> const ColorToken* {
        if (emphasized(t)) return &COLOR_LIST_HOVER;
        if (selection_.count(t)) return &COLOR_SELECTED;
        if (hovered(t)) return &COLOR_HOVER;
        return nullptr;
    };
    for (const auto& pd : nested_.planes) {
        const ColorToken* m = marked(nestedTarget(PickTarget::Kind::Plane, pd.id));
        if (!nestedPlaneShown(pd.id) && !m) continue;
        const QPolygonF border = nestedPlaneQuad(pd);
        p.setPen(QPen(m ? m->color : COLOR_PLANE_BORDER.color, m ? EDGE_SELECTED_WIDTH_PX : 1));
        p.drawPolygon(border);
        p.drawText(border[3] + QPointF(4, 14), pd.label);
    }
    for (const auto& a : nested_.axes) {
        const ColorToken* m = marked(nestedTarget(PickTarget::Kind::Axis, a.id));
        p.setPen(QPen(m ? m->color : COLOR_AXIS_REF.color, m ? EDGE_SELECTED_WIDTH_PX : 1, Qt::DashDotLine));
        const QVector3D d = a.dir * float(planeHalf_);
        p.drawLine(camera_.project(a.point - d), camera_.project(a.point + d));
    }
    p.setPen(QPen(COLOR_SK_FULL.color, SK_LINE_PX));
    for (const auto& line : nested_.sketchLines) {
        QPolygonF pl;
        for (const auto& v : line) pl << camera_.project(v);
        p.drawPolyline(pl);
    }
}

void Viewport::setPlaneHalf(double h) {
    if (planeHalf_ == h) return;
    planeHalf_ = h;
    ++sceneGen_;
    dirty_ = true;
    update();
}

void Viewport::setAxesVisible(bool v) {
    if (axesVisible_ == v) return;
    axesVisible_ = v;
    ++sceneGen_;
    dirty_ = true;
    update();
}

bool Viewport::axesVisible() const { return axesVisible_; }

void Viewport::setRegions(std::vector<DrawRegion> regions, bool onTop) {
    auto same = [](const DrawRegion& a, const DrawRegion& b) {
        return a.sketch == b.sketch && a.regionId == b.regionId && a.triangles == b.triangles;
    };
    if (onTop == regionsOnTop_ && regions.size() == regions_.size() &&
        std::equal(regions.begin(), regions.end(), regions_.begin(), same))
        return;  // unchanged — the frame is not redrawn
    regions_ = std::move(regions);
    regionsOnTop_ = onTop;
    ++sceneGen_;
    dirty_ = true;
    update();
}

void Viewport::setHover(const PickTarget& t) {
    if (hover_ == t) return;
    hover_ = t;
    dirty_ = true;
    update();
}

void Viewport::setSelection(const std::set<PickTarget>& s) {
    if (selection_ == s) return;
    selection_ = s;
    ++sceneGen_;  // the selected origin plane is visible and selectable (RenderScene::planeShown)
    dirty_ = true;
    update();
    emit selectionChanged();
}

// ---------------------------------------------------------------- GL

QString aboutGlText(const QString& renderer, const QString& autoSoftwareVersion) {
    if (renderer.isEmpty()) return text("ui.about.glUnknown");
    if (!autoSoftwareVersion.isEmpty()) return text("ui.about.glSoftwareAuto", {{"renderer", renderer}, {"version", autoSoftwareVersion}});
    return text(isSoftwareGlRenderer(renderer) ? "ui.about.glSoftware" : "ui.about.glHardware", {{"renderer", renderer}});
}

bool isSoftwareGlRenderer(const QString& renderer) {
    for (const char* k : {"llvmpipe", "softpipe", "swrast", "GDI Generic", "Basic Render"})
        if (renderer.contains(QLatin1String(k), Qt::CaseInsensitive)) return true;
    return false;
}

void Viewport::initializeGL() {
    glRenderer_ = QString::fromLatin1(reinterpret_cast<const char*>(context()->functions()->glGetString(GL_RENDERER)));
    log::info(QString("OpenGL: %1, %2 (%3)")
                  .arg(glRenderer_, QString::fromLatin1(reinterpret_cast<const char*>(context()->functions()->glGetString(GL_VERSION))),
                       isSoftwareGlRenderer(glRenderer_) ? "software" : "hardware")
                  .toStdString());
    // Automatic fallback to software OpenGL (client.md §2а, FX3-PERF-004).
    if (*glAutoSoftwareVersion())
        log::info(QString("OpenGL: видеокарта даёт OpenGL %1, нужен 3.0 — включён программный OpenGL").arg(glAutoSoftwareVersion()).toStdString());  // i18n-skip: log
    // OpenGL 3.3 core (normal mode) or 3.0 (Mesa software rendering).
    const QSurfaceFormat f = context()->format();
    log::info(QString("OpenGL: context %1.%2 %3").arg(f.majorVersion()).arg(f.minorVersion())
                  .arg(f.profile() == QSurfaceFormat::CoreProfile ? "core" : f.profile() == QSurfaceFormat::CompatibilityProfile ? "compatibility" : "no profile")
                  .toStdString());
    renderer_.initialize(f.profile() == QSurfaceFormat::CoreProfile && (f.majorVersion() > 3 || (f.majorVersion() == 3 && f.minorVersion() >= 3)));
}

void Viewport::resizeGL(int w, int h) {
    camera_.viewport = QSize(w, h);
    renderer_.resetIdBuffer();
    dirty_ = true;
    emit cameraChanged();
}

void Viewport::updateDepthRange() {
    QVector3D mn, mx;
    double r = planeHalf_ * 2;
    if (sceneBounds(mn, mx)) r = std::max(r, double((mx - mn).length()));
    const double off = sceneBounds(mn, mx) ? double(((mn + mx) * 0.5f - camera_.center).length()) : 0.0;
    camera_.depthRange = std::max(1000.0, 4 * (r + off));
}

RenderScene Viewport::renderScene() const {
    return RenderScene{camera_, bodies_, planesVisible_, planeHalf_, regions_, regionsOnTop_, hover_, selection_, shownPlanes_, planes_,
                       allPlanesShown_, style_, &treeHover_, treeHoverEmphasis_, float(devicePixelRatioF())};
}

void Viewport::ensureIdBuffer() const {
    if (inPaint_ || !isValid() || !renderer_.ready()) return;
    auto* self = const_cast<Viewport*>(this);
    self->updateDepthRange();
    const QSize sz = size() * devicePixelRatioF();
    const RenderScene scene = renderScene();
    const QMatrix4x4 vp = scene.camera.viewProjection();
    if (idGen_ == sceneGen_ && idSize_ == sz && idViewProj_ == vp && !idImage_.isNull()) return;
    self->makeCurrent();
    if (bodiesDirty_) {
        self->renderer_.uploadBodies(bodies_);
        self->bodiesDirty_ = false;
    }
    IdFrame idf = self->renderer_.renderIdPass(scene, sz);
    self->doneCurrent();
    idImage_ = std::move(idf.image);
    depth_ = std::move(idf.depth);
    pickTable_ = std::move(idf.table);
    idGen_ = sceneGen_;
    idSize_ = sz;
    idViewProj_ = vp;
}

void Viewport::paintGL() {
    inPaint_ = true;
    if (bodiesDirty_) {
        renderer_.uploadBodies(bodies_);
        bodiesDirty_ = false;
    }
    updateDepthRange();
    const QSize sz = size() * devicePixelRatioF();
    const RenderScene scene = renderScene();
    renderer_.renderColor(scene, sz, defaultFramebufferObject());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (planesVisible_) {
        p.setPen(COLOR_PLANE_BORDER.color);
        for (const auto& pd : planes_) {
            if (!scene.planeShown(pd.id)) continue;
            const QVector3D corner = pd.center + (q(pd.frame.x) * -1 + q(pd.frame.y)) * float(planeHalf_);
            p.drawText(camera_.project(corner) + QPointF(4, 14), pd.label);
        }
    }
    drawNestedReference(p);
    drawOrigin(p);
    drawDatumsAndVertices(p);
    if (tool_) tool_->paint(p);
    p.end();
    inPaint_ = false;
    dirty_ = false;
    ++frame_;
    emit framePainted();
}

void Viewport::drawOrigin(QPainter& p) {
    const QPointF o = camera_.project(QVector3D(0, 0, 0));
    const ColorToken* cols[3] = {&COLOR_AXIS_X, &COLOR_AXIS_Y, &COLOR_AXIS_Z};
    const QVector3D axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i) {
        const QPointF e = camera_.project(axes[i] * float(camera_.mmPerPx() * ORIGIN_TRIAD_PX));
        p.setPen(QPen(cols[i]->color, 2));
        p.drawLine(o, e);
    }
}

std::optional<std::pair<QPointF, QPointF>> Viewport::axisSegmentOf(const std::string& id) const {
    for (int i = 0; i < 3; ++i)
        if (id == DATUM_AXES[i].id) return axisSegment(i);
    for (const auto& a : refAxes_)
        if (a.id == id) {
            const QVector3D d = a.dir * float(planeHalf_);
            return std::pair<QPointF, QPointF>{camera_.project(a.point - d), camera_.project(a.point + d)};
        }
    return std::nullopt;
}

std::pair<QPointF, QPointF> Viewport::axisSegment(int i) const {
    QVector3D a(0, 0, 0);
    a[i] = float(planeHalf_);
    return {camera_.project(-a), camera_.project(a)};
}

void Viewport::drawDatumsAndVertices(QPainter& p) {
    auto marked = [&](const PickTarget& t) -> const ColorToken* {
        if (emphasized(t)) return &COLOR_LIST_HOVER;
        if (selection_.count(t)) return &COLOR_SELECTED;
        if (hovered(t)) return &COLOR_HOVER;
        return nullptr;
    };
    // Axes: visible on request, plus the hovered/selected one.
    const ColorToken* cols[3] = {&COLOR_AXIS_X, &COLOR_AXIS_Y, &COLOR_AXIS_Z};
    for (int i = 0; i < 3; ++i) {
        const PickTarget t{PickTarget::Kind::Axis, {}, DATUM_AXES[i].id};
        const ColorToken* m = marked(t);
        if (!axesVisible_ && !m) continue;
        const auto [a, b] = axisSegment(i);
        p.setPen(QPen(m ? m->color : cols[i]->color, m ? EDGE_SELECTED_WIDTH_PX : 1, Qt::DashDotLine));
        p.drawLine(a, b);
    }
    // Reference axes are always visible (§3.4, S2-REF-004).
    for (const auto& a : refAxes_) {
        const PickTarget t{PickTarget::Kind::Axis, {}, a.id};
        const ColorToken* m = marked(t);
        const auto [s0, s1] = *axisSegmentOf(a.id);
        p.setPen(QPen(m ? m->color : COLOR_AXIS_REF.color, m ? EDGE_SELECTED_WIDTH_PX : 1, Qt::DashDotLine));
        p.drawLine(s0, s1);
    }
    // Origin and body vertices — a small circle on hover/selection.
    auto ring = [&](const QPointF& c, const ColorToken* m) {
        const double r = (m == &COLOR_SELECTED ? VERTEX_SELECTED_PX : VERTEX_HOVER_PX) / 2.0;
        p.setPen(QPen(m->color, 2));
        p.setBrush(m->color);
        p.drawEllipse(c, r, r);
        p.setBrush(Qt::NoBrush);
    };
    if (const ColorToken* m = marked({PickTarget::Kind::Origin, {}, "origin"})) ring(camera_.project(QVector3D(0, 0, 0)), m);
    for (const auto& b : bodies_)
        for (const auto& v : b.mesh.vertices)
            if (const ColorToken* m = marked({PickTarget::Kind::Vertex, b.mesh.bodyId, v.name}))
                ring(camera_.project(QVector3D(v.position[0], v.position[1], v.position[2])), m);
}

// ---------------------------------------------------------------- picking

bool Viewport::pointVisible(const QVector3D& w) const {
    // A point is visible if the ray from it to the camera does not hit opaque bodies (Möller–Trumbore).
    // Triangles incident to the point give a hit at the point itself — cut off by a tolerance.
    QVector3D o, dir;
    camera_.ray(camera_.project(w), o, dir);
    const QVector3D toCam = -dir.normalized();
    const float eps = float(std::max(1e-6, camera_.mmPerPx() * 0.5));
    for (const auto& b : bodies_) {
        if (b.alpha < 1.0f) continue;  // semi-transparent ones (preview) do not occlude
        const auto& P = b.mesh.positions;
        const auto& T = b.mesh.triangles;
        for (size_t i = 0; i + 2 < T.size(); i += 3) {
            const QVector3D a(P[3 * T[i]], P[3 * T[i] + 1], P[3 * T[i] + 2]);
            const QVector3D e1 = QVector3D(P[3 * T[i + 1]], P[3 * T[i + 1] + 1], P[3 * T[i + 1] + 2]) - a;
            const QVector3D e2 = QVector3D(P[3 * T[i + 2]], P[3 * T[i + 2] + 1], P[3 * T[i + 2] + 2]) - a;
            const QVector3D pv = QVector3D::crossProduct(toCam, e2);
            const float det = QVector3D::dotProduct(e1, pv);
            if (std::abs(det) < 1e-12f) continue;
            const QVector3D tv = w - a;
            const float u = QVector3D::dotProduct(tv, pv) / det;
            if (u < 0 || u > 1) continue;
            const QVector3D qv = QVector3D::crossProduct(tv, e1);
            const float v = QVector3D::dotProduct(toCam, qv) / det;
            if (v < 0 || u + v > 1) continue;
            if (QVector3D::dotProduct(e2, qv) / det > eps) return false;
        }
    }
    return true;
}

std::optional<std::pair<double, PickTarget>> Viewport::pickVertex(const QPointF& s) const {
    std::optional<std::pair<double, PickTarget>> best;
    for (const auto& b : bodies_) {
        if (!b.pickable) continue;
        for (const auto& v : b.mesh.vertices) {
            const QVector3D w(v.position[0], v.position[1], v.position[2]);
            const QPointF ps = camera_.project(w);
            const double d = std::hypot(ps.x() - s.x(), ps.y() - s.y());
            if (d > PICK_POINT_PX) continue;
            const PickTarget t{PickTarget::Kind::Vertex, b.mesh.bodyId, v.name};
            if (best && (d > best->first || (d == best->first && !(t.name < best->second.name)))) continue;
            if (!pointVisible(w)) continue;
            best = {{d, t}};
        }
    }
    return best;
}

void Viewport::setSketchPicks(std::vector<DrawSketchPick> picks) {
    sketchPicks_ = std::move(picks);
    update();
}

std::optional<std::pair<double, PickTarget>> Viewport::pickSketch(const QPointF& s, bool curves) const {
    std::optional<std::pair<double, PickTarget>> best;
    for (const auto& p : sketchPicks_) {
        if ((p.kind == DrawSketchPick::Kind::Point) == curves || p.points.empty()) continue;
        double d = 1e300;
        if (p.kind == DrawSketchPick::Kind::Point) {
            const QPointF q = camera_.project(p.points[0]);
            d = std::hypot(q.x() - s.x(), q.y() - s.y());
        } else {
            for (size_t i = 0; i + 1 < p.points.size(); ++i) d = std::min(d, segDist(s, camera_.project(p.points[i]), camera_.project(p.points[i + 1])));
        }
        if (d > (curves ? PICK_CURVE_PX : PICK_POINT_PX)) continue;
        const PickTarget t{PickTarget::Kind::SketchEntity, p.sketch, p.entity};
        if (!best || d < best->first) best = {{d, t}};
    }
    return best;
}

std::optional<PickTarget> Viewport::pickDatum(const QPointF& s) const {
    // Within priority 6: the point (origin) goes before axes (app-ux §3.5).
    const QPointF o = camera_.project(QVector3D(0, 0, 0));
    if (std::hypot(o.x() - s.x(), o.y() - s.y()) <= PICK_POINT_PX) return PickTarget{PickTarget::Kind::Origin, {}, "origin"};
    std::optional<std::pair<double, PickTarget>> best;
    for (int i = 0; i < 3; ++i) {
        const PickTarget t{PickTarget::Kind::Axis, {}, DATUM_AXES[i].id};
        if (!axesVisible_ && !hovered(t) && !selection_.count(t)) continue;
        const auto [a, b] = axisSegment(i);
        const double d = segDist(s, a, b);
        if (d <= PICK_CURVE_PX && (!best || d < best->first)) best = {{d, t}};
    }
    for (const auto& a : refAxes_) {
        const auto [s0, s1] = *axisSegmentOf(a.id);
        const double d = segDist(s, s0, s1);
        if (d <= PICK_CURVE_PX && (!best || d < best->first)) best = {{d, PickTarget{PickTarget::Kind::Axis, {}, a.id}}};
    }
    if (nestedPickable_)  // axes of nested subassemblies (FX9-ASM-014)
        for (const auto& a : nested_.axes) {
            const QVector3D dv = a.dir * float(planeHalf_);
            const double d = segDist(s, camera_.project(a.point - dv), camera_.project(a.point + dv));
            if (d <= PICK_CURVE_PX && (!best || d < best->first)) best = {{d, nestedTarget(PickTarget::Kind::Axis, a.id)}};
        }
    if (best) return best->second;
    return std::nullopt;
}

std::string Viewport::refAxisAt(const QPoint& p) const {
    std::string best;
    double bestD = PICK_CURVE_PX;
    for (const auto& a : refAxes_) {
        const auto seg = axisSegmentOf(a.id);
        if (!seg) continue;
        const double d = segDist(QPointF(p), seg->first, seg->second);
        if (d <= bestD) bestD = d, best = a.id;
    }
    return best;
}

PickTarget Viewport::pickAt(const QPoint& pos, bool facesAllowed, bool edgesAllowed, bool planesAllowed) const {
    ensureIdBuffer();
    if (idImage_.isNull()) return {};
    const double dpr = devicePixelRatioF();
    const int cx = int(pos.x() * dpr), cy = int(pos.y() * dpr);
    const int R = int(PICK_WINDOW_PX * dpr);
    std::optional<std::pair<double, PickTarget>> bestEdge, bestFace, bestPlane, bestRegion, bestWeld;
    for (int y = cy - R; y <= cy + R; ++y) {
        if (y < 0 || y >= idImage_.height()) continue;
        const uchar* row = idImage_.constScanLine(y);
        for (int x = cx - R; x <= cx + R; ++x) {
            if (x < 0 || x >= idImage_.width()) continue;
            const uchar* px = row + 4 * x;
            const uint32_t id = uint32_t(px[0]) | (uint32_t(px[1]) << 8) | (uint32_t(px[2]) << 16);
            if (id == 0 || id > pickTable_.size()) continue;
            const PickTarget& t = pickTable_[id - 1];
            const double d = std::hypot(x - cx, y - cy) / dpr;
            auto consider = [&](std::optional<std::pair<double, PickTarget>>& best, double tol) {
                if (d > tol) return;
                if (!best || d < best->first || (d == best->first && t.name < best->second.name)) best = {{d, t}};
            };
            switch (t.kind) {
                case PickTarget::Kind::Edge: if (edgesAllowed) consider(bestEdge, PICK_CURVE_PX); break;
                case PickTarget::Kind::Face: if (facesAllowed) consider(bestFace, 0.5); break;
                case PickTarget::Kind::Plane: if (planesAllowed) consider(bestPlane, 0.5); break;
                case PickTarget::Kind::Region: if (facesAllowed) consider(bestRegion, 0.5); break;
                case PickTarget::Kind::Weld: if (weldsPickable_ && edgesAllowed) consider(bestWeld, 0.5); break;
                default: break;
            }
        }
    }
    if (bestWeld) return bestWeld->second;  // the weld body under the cursor — before body elements (app-ux §8а, S7F-WLD-001)
    if (edgesAllowed) {
        // Body vertices and sketch points (§3.5 priority 2), then edges and sketch curves (priority 3): the nearest.
        auto v = pickVertex(QPointF(pos));
        if (auto sp = pickSketch(QPointF(pos), false); sp && (!v || sp->first < v->first)) v = sp;
        if (v) return v->second;
        if (auto sc = pickSketch(QPointF(pos), true); sc && (!bestEdge || sc->first < bestEdge->first)) return sc->second;
    }
    if (bestEdge) return bestEdge->second;
    if (bestRegion) return bestRegion->second;
    if (bestFace) return bestFace->second;
    if (facesAllowed)
        if (auto d = pickDatum(QPointF(pos))) return *d;
    if (bestPlane) return bestPlane->second;
    if (planesAllowed && nestedPickable_)  // shown planes of nested subassemblies (FX9-ASM-014): inside the border
        for (const auto& pd : nested_.planes)
            if (nestedPlaneShown(pd.id) && nestedPlaneQuad(pd).containsPoint(QPointF(pos), Qt::OddEvenFill))
                return nestedTarget(PickTarget::Kind::Plane, pd.id);
    return {};
}

std::optional<QVector3D> Viewport::pointUnder(const QPoint& pos) const {
    ensureIdBuffer();
    if (depth_.empty() || idImage_.isNull()) return std::nullopt;
    const double dpr = devicePixelRatioF();
    const int x = int(pos.x() * dpr), y = int(pos.y() * dpr);
    if (x < 0 || y < 0 || x >= idImage_.width() || y >= idImage_.height()) return std::nullopt;
    const float d = depth_[size_t(idImage_.height() - 1 - y) * idImage_.width() + x];
    if (d >= 1.0f) return std::nullopt;
    return camera_.unproject(QPointF(pos), d);
}

std::optional<QPoint> Viewport::screenPosOf(const PickTarget& t) const {
    ensureIdBuffer();
    if (idImage_.isNull()) return std::nullopt;
    if (t.kind == PickTarget::Kind::SketchEntity) {  // a point of the element where a click picks it (§12.1)
        for (const auto& p : sketchPicks_) {
            if (p.sketch != t.body || p.entity != t.name || p.points.empty()) continue;
            std::vector<QPointF> cands;
            if (p.kind == DrawSketchPick::Kind::Point) cands.push_back(camera_.project(p.points[0]));
            for (size_t i = 0; i + 1 < p.points.size(); ++i)
                for (double f : {0.5, 0.3, 0.7}) cands.push_back(camera_.project(p.points[i] + (p.points[i + 1] - p.points[i]) * float(f)));
            for (const auto& c : cands) {
                const QPoint lp(int(std::lround(c.x())), int(std::lround(c.y())));
                if (rect().contains(lp) && pickAt(lp) == t) return lp;
            }
        }
        return std::nullopt;
    }
    if (const DrawPlane* pd = nestedPlaneOf(t)) {  // FX9-ASM-014: points inside the border
        const QPolygonF quad = nestedPlaneQuad(*pd);
        for (double fx : {0.5, 0.3, 0.7, 0.15, 0.85})
            for (double fy : {0.5, 0.3, 0.7, 0.15, 0.85}) {
                const QPointF c = quad[0] + (quad[1] - quad[0]) * fx + (quad[3] - quad[0]) * fy;
                const QPoint lp(int(std::lround(c.x())), int(std::lround(c.y())));
                if (rect().contains(lp) && pickAt(lp) == t) return lp;
            }
        return std::nullopt;
    }
    if (const DrawAxis* na = nestedAxisOf(t)) {  // FX9-ASM-014
        const QVector3D dv = na->dir * float(planeHalf_);
        const QPointF a = camera_.project(na->point - dv), b = camera_.project(na->point + dv);
        for (int k = 1; k < 64; ++k) {
            const double f = (k % 2 ? 32 + (k + 1) / 2 : 32 - k / 2) / 64.0;  // from the middle outwards
            const QPointF c = a + (b - a) * f;
            const QPoint lp(int(std::lround(c.x())), int(std::lround(c.y())));
            if (rect().contains(lp) && pickAt(lp) == t) return lp;
        }
        return std::nullopt;
    }
    if (t.kind == PickTarget::Kind::Vertex || t.kind == PickTarget::Kind::Origin || t.kind == PickTarget::Kind::Axis) {
        std::vector<QPointF> cands;
        if (t.kind == PickTarget::Kind::Origin) cands.push_back(camera_.project(QVector3D(0, 0, 0)));
        if (t.kind == PickTarget::Kind::Axis)
            if (const auto seg = axisSegmentOf(t.name)) {
                const auto [a, b] = *seg;
                for (double f : {0.85, 0.15, 0.7, 0.3, 0.95, 0.05}) cands.push_back(a + (b - a) * f);
                // Strongly zoomed in, these points are past the view edge — then any point of the axis visible part.
                for (int k = 1; k < 64; ++k) cands.push_back(a + (b - a) * (k / 64.0));
            }
        for (const auto& b : bodies_)
            if (b.mesh.bodyId == t.body)
                for (const auto& v : b.mesh.vertices)
                    if (t.kind == PickTarget::Kind::Vertex && v.name == t.name) cands.push_back(camera_.project(QVector3D(v.position[0], v.position[1], v.position[2])));
        for (const auto& c : cands) {
            const QPoint lp(int(std::lround(c.x())), int(std::lround(c.y())));
            if (rect().contains(lp) && pickAt(lp) == t) return lp;
        }
        return std::nullopt;
    }
    uint32_t id = 0;
    for (size_t i = 0; i < pickTable_.size(); ++i)
        if (pickTable_[i] == t) id = uint32_t(i + 1);
    if (!id) return std::nullopt;
    std::vector<QPoint> pts;
    for (int y = 0; y < idImage_.height(); ++y) {
        const uchar* row = idImage_.constScanLine(y);
        for (int x = 0; x < idImage_.width(); ++x) {
            const uchar* px = row + 4 * x;
            if ((uint32_t(px[0]) | (uint32_t(px[1]) << 8) | (uint32_t(px[2]) << 16)) == id) pts.push_back({x, y});
        }
    }
    if (pts.empty()) return std::nullopt;
    double sx = 0, sy = 0;
    for (const auto& p : pts) sx += p.x(), sy += p.y();
    const QPointF c(sx / pts.size(), sy / pts.size());
    const double dpr = devicePixelRatioF();
    // Valid pixel: a click on it selects exactly the target.
    std::sort(pts.begin(), pts.end(), [&](const QPoint& a, const QPoint& b) {
        const double da = std::hypot(a.x() - c.x(), a.y() - c.y()), db = std::hypot(b.x() - c.x(), b.y() - c.y());
        if (da != db) return da < db;
        return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x();
    });
    for (const auto& p : pts) {
        const QPoint lp(int(std::lround(p.x() / dpr)), int(std::lround(p.y() / dpr)));
        if (pickAt(lp) == t) return lp;
    }
    return std::nullopt;
}

bool Viewport::sceneBounds(QVector3D& mn, QVector3D& mx) const {
    bool any = false;
    for (const auto& b : bodies_) {
        for (size_t i = 0; i + 2 < b.mesh.positions.size(); i += 3) {
            const QVector3D p(b.mesh.positions[i], b.mesh.positions[i + 1], b.mesh.positions[i + 2]);
            if (!any) mn = mx = p, any = true;
            mn = QVector3D(std::min(mn.x(), p.x()), std::min(mn.y(), p.y()), std::min(mn.z(), p.z()));
            mx = QVector3D(std::max(mx.x(), p.x()), std::max(mx.y(), p.y()), std::max(mx.z(), p.z()));
        }
    }
    return any;
}

Camera Viewport::fitted(Camera c) const {
    QVector3D mn, mx;
    if (!sceneBounds(mn, mx)) {
        const float h = float(planeHalf_);
        mn = QVector3D(-h, -h, -h);
        mx = QVector3D(h, h, h);
    }
    c.fit(mn, mx);
    return c;
}

bool Viewport::targetBounds(const std::set<PickTarget>& targets, QVector3D& mn, QVector3D& mx) const {
    bool any = false;
    auto add = [&](const QVector3D& p) {
        if (!any) mn = mx = p, any = true;
        mn = QVector3D(std::min(mn.x(), p.x()), std::min(mn.y(), p.y()), std::min(mn.z(), p.z()));
        mx = QVector3D(std::max(mx.x(), p.x()), std::max(mx.y(), p.y()), std::max(mx.z(), p.z()));
    };
    auto pos = [](const io::BodyMesh& m, uint32_t v) { return QVector3D(m.positions[3 * v], m.positions[3 * v + 1], m.positions[3 * v + 2]); };
    for (const PickTarget& t : targets) {
        // A face, an edge or a vertex: the body may be unknown (body empty) — the first body with the name.
        if (t.kind == PickTarget::Kind::Face || t.kind == PickTarget::Kind::Edge || t.kind == PickTarget::Kind::Vertex)
            for (const auto& b : bodies_) {
                if (!t.body.empty() && b.mesh.bodyId != t.body) continue;
                bool found = false;
                if (t.kind == PickTarget::Kind::Face)
                    for (const auto& fr : b.mesh.faceRanges) {
                        if (fr.name != t.name) continue;
                        for (uint32_t k = fr.firstTriangle; k < fr.firstTriangle + fr.count && 3 * k + 2 < b.mesh.triangles.size(); ++k)
                            for (int c = 0; c < 3; ++c) add(pos(b.mesh, b.mesh.triangles[3 * k + c]));
                        found = true;
                    }
                if (t.kind == PickTarget::Kind::Edge)
                    for (const auto& e : b.mesh.edges) {
                        if (e.name != t.name) continue;
                        for (const auto& pl : e.polylines)
                            for (size_t i = 0; i + 2 < pl.size(); i += 3) add(QVector3D(pl[i], pl[i + 1], pl[i + 2]));
                        found = true;
                    }
                if (t.kind == PickTarget::Kind::Vertex)
                    for (const auto& v : b.mesh.vertices)
                        if (v.name == t.name) add(QVector3D(v.position[0], v.position[1], v.position[2])), found = true;
                if (found) break;
            }
        if (t.kind == PickTarget::Kind::Origin) add(QVector3D(0, 0, 0));
        if (t.kind == PickTarget::Kind::Plane)
            for (const auto& pd : planes_) {
                if (pd.id != t.name) continue;
                auto q = [](const Vec3& v) { return QVector3D(float(v.x()), float(v.y()), float(v.z())); };
                const QVector3D x = q(pd.frame.x) * float(planeHalf_), y = q(pd.frame.y) * float(planeHalf_);
                for (float sx : {-1.0f, 1.0f})
                    for (float sy : {-1.0f, 1.0f}) add(pd.center + x * sx + y * sy);
            }
        if (t.kind == PickTarget::Kind::Axis) {
            for (int i = 0; i < 3; ++i)
                if (t.name == DATUM_AXES[i].id) {
                    QVector3D a(0, 0, 0);
                    a[i] = float(planeHalf_);
                    add(-a), add(a);
                }
            for (const auto& a : refAxes_)
                if (a.id == t.name) add(a.point - a.dir * float(planeHalf_)), add(a.point + a.dir * float(planeHalf_));
        }
        if (t.kind == PickTarget::Kind::Region)
            for (const auto& r : regions_)
                if (r.sketch == t.body && (t.name.empty() || r.regionId == t.name))
                    for (const auto& p : r.triangles) add(p);
        if (t.kind == PickTarget::Kind::SketchEntity)
            for (const auto& sp : sketchPicks_)
                if (sp.sketch == t.body && (t.name.empty() || sp.entity == t.name))
                    for (const auto& p : sp.points) add(p);
        if (t.kind == PickTarget::Kind::Weld)
            for (const auto& w : welds_)
                if (w.id == t.name)
                    for (const auto& pl : w.polylines)
                        for (const auto& p : pl) add(p);
    }
    return any;
}

bool Viewport::centerOn(const std::set<PickTarget>& targets, bool animated) {
    QVector3D mn, mx;
    if (!targetBounds(targets, mn, mx)) return false;
    Camera c = targetCamera();
    c.frameOn(mn, mx, SELECTION_FRAME_FRACTION);
    if (animated && animMs_ > 0) animateTo(c);
    else {
        stopAnimation();
        setCamera(c);
    }
    return true;
}

void Viewport::fitAll(bool animated) {
    const Camera c = fitted(targetCamera());
    if (animated) {
        animateTo(c);
        return;
    }
    stopAnimation();
    setCamera(c);
}

QVector3D Viewport::rotationPivot(const QPoint& p) const {
    // The origin plane is the rotation center only without bodies (§3.1): it is larger than the part, rotation around
    // its far point moves the part out of view.
    bool planeUnder = false;
    ensureIdBuffer();
    if (!bodies_.empty() && !idImage_.isNull()) {
        const double dpr = devicePixelRatioF();
        const int x = int(p.x() * dpr), y = int(p.y() * dpr);
        if (x >= 0 && y >= 0 && x < idImage_.width() && y < idImage_.height()) {
            const uchar* px = idImage_.constScanLine(y) + 4 * x;
            const uint32_t id = uint32_t(px[0]) | (uint32_t(px[1]) << 8) | (uint32_t(px[2]) << 16);
            planeUnder = id > 0 && id <= pickTable_.size() && pickTable_[id - 1].kind == PickTarget::Kind::Plane;
        }
    }
    if (!planeUnder)
        if (auto w = pointUnder(p)) return *w;
    QVector3D mn, mx;
    if (sceneBounds(mn, mx)) return (mn + mx) * 0.5f;
    return QVector3D(0, 0, 0);
}

// ---------------------------------------------------------------- input (§3.1, mouse profiles)

void Viewport::mousePressEvent(QMouseEvent* e) {
    stopAnimation();  // input interrupts a camera transition (§3.3)
    const bool rightRotate = mouseProfile_ == MouseProfile::RightButton;
    if (e->button() == Qt::MiddleButton || (rightRotate && e->button() == Qt::RightButton)) {
        if (e->button() == Qt::RightButton) rightPress_ = e->position();
        gestureArmed_ = true;
        gestureButton_ = e->button();
        gestureMods_ = e->modifiers();
        pressPos_ = lastPos_ = e->position().toPoint();
        cam0_ = camera_;
        pivot_ = rotationPivot(pressPos_);
        return;
    }
    if (tool_ && tool_->mousePress(e)) return;
    QOpenGLWidget::mousePressEvent(e);
}

namespace {
// The focus is in an input field — a line or a multi-line text (§12.3; the text of a sketch inscription, FX14-UI-001:
// the closing symbols menu leaves the cursor over the view).
bool inputFieldFocused() {
    QWidget* f = QApplication::focusWidget();
    return f && (f->inherits("QLineEdit") || f->inherits("QTextEdit") || f->inherits("QPlainTextEdit"));
}
}  // namespace

void Viewport::mouseMoveEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    // §12.3: the view takes focus on hover unless focus is in an input field.
    if (!hasFocus() && !inputFieldFocused()) setFocus(Qt::MouseFocusReason);
    if (gestureArmed_ && gesture_ == Gesture::None && (pos - pressPos_).manhattanLength() > DRAG_THRESHOLD_PX) {
        // Gesture kind — by the button and the modifiers at press time (§3.1).
        if (mouseProfile_ == MouseProfile::RightButton && gestureButton_ == Qt::MiddleButton) gesture_ = Gesture::Pan;
        else
            gesture_ = (gestureMods_ & Qt::ControlModifier) ? Gesture::Pan
                       : (gestureMods_ & Qt::ShiftModifier) ? Gesture::Zoom
                                                            : Gesture::Rotate;
        rightPress_.reset();
    }
    if (gesture_ != Gesture::None) {
        if (gesture_ == Gesture::Rotate) {
            Camera c = Camera::rotated(cam0_, QPointF(pos - pressPos_), pivot_);
            c.viewport = camera_.viewport;
            camera_ = c;
        } else if (gesture_ == Gesture::Pan) {
            camera_.panPixels(QPointF(pos - lastPos_));
        } else {
            camera_.zoomAt(QPointF(pressPos_), std::pow(ZOOM_STEP_FACTOR, -(pos.y() - lastPos_.y()) / 20.0));
        }
        lastPos_ = pos;
        dirty_ = true;
        update();
        emit cameraChanged();
        return;
    }
    if (tool_ && tool_->mouseMove(e)) return;
    QOpenGLWidget::mouseMoveEvent(e);
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == gestureButton_ && gestureArmed_) {
        gestureArmed_ = false;
        gesture_ = Gesture::None;
        gestureButton_ = Qt::NoButton;
        // RightButton profile: RMB without a gesture — an RMB click for the tool (context menu); no menu after a gesture.
        if (rightPress_ && tool_) {
            QMouseEvent press(QEvent::MouseButtonPress, *rightPress_, mapToGlobal(*rightPress_), Qt::RightButton, Qt::RightButton,
                              e->modifiers());
            tool_->mousePress(&press);
            tool_->mouseRelease(e);
        }
        rightPress_.reset();
        return;
    }
    if (tool_ && tool_->mouseRelease(e)) return;
    QOpenGLWidget::mouseReleaseEvent(e);
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
    if (tool_ && tool_->mouseDoubleClick(e)) return;
    QOpenGLWidget::mouseDoubleClickEvent(e);
}

void Viewport::wheelEvent(QWheelEvent* e) {
    stopAnimation();
    const double steps = (invertWheel_ ? -1 : 1) * e->angleDelta().y() / 120.0;
    camera_.zoomAt(e->position(), std::pow(ZOOM_STEP_FACTOR, steps));
    dirty_ = true;
    update();
    emit cameraChanged();
}

void Viewport::keyPressEvent(QKeyEvent* e) {
    if (tool_ && tool_->keyPress(e)) return;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool alt = e->modifiers() & Qt::AltModifier;
    const QVector3D pivot = camera_.center;
    const double deg = shift ? 90 : KEY_ROTATE_DEG;
    bool handled = true;
    const int pk = physicalKey(e);
    if (pk == Qt::Key_Left || pk == Qt::Key_Right || pk == Qt::Key_Up || pk == Qt::Key_Down || pk == Qt::Key_Z) stopAnimation();
    switch (pk) {
        case Qt::Key_Left:
            if (ctrl) camera_.panPixels(QPointF(-camera_.viewport.width() * KEY_PAN_FRACTION, 0));
            else if (alt) camera_.rotateScreen(0, 0, -KEY_ROTATE_DEG, pivot);
            else camera_.rotateScreen(-deg, 0, 0, pivot);
            break;
        case Qt::Key_Right:
            if (ctrl) camera_.panPixels(QPointF(camera_.viewport.width() * KEY_PAN_FRACTION, 0));
            else if (alt) camera_.rotateScreen(0, 0, KEY_ROTATE_DEG, pivot);
            else camera_.rotateScreen(deg, 0, 0, pivot);
            break;
        case Qt::Key_Up:
            if (ctrl) camera_.panPixels(QPointF(0, -camera_.viewport.height() * KEY_PAN_FRACTION));
            else camera_.rotateScreen(0, -deg, 0, pivot);
            break;
        case Qt::Key_Down:
            if (ctrl) camera_.panPixels(QPointF(0, camera_.viewport.height() * KEY_PAN_FRACTION));
            else camera_.rotateScreen(0, deg, 0, pivot);
            break;
        case Qt::Key_Z: {
            const QPointF c(camera_.viewport.width() / 2.0, camera_.viewport.height() / 2.0);
            camera_.zoomAt(c, shift ? ZOOM_STEP_FACTOR : 1 / ZOOM_STEP_FACTOR);
            break;
        }
        case Qt::Key_F: fitAll(true); return;
        default: handled = false;
    }
    if (!handled) {
        QOpenGLWidget::keyPressEvent(e);
        return;
    }
    dirty_ = true;
    update();
    emit cameraChanged();
}

void Viewport::enterEvent(QEnterEvent* e) {
    // §12.3: the view takes focus on hover unless focus is in an input field.
    if (!inputFieldFocused()) setFocus(Qt::MouseFocusReason);
    QOpenGLWidget::enterEvent(e);
}

bool Viewport::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip && tool_) {  // tool tooltip (app-ux §6.6)
        auto* he = static_cast<QHelpEvent*>(e);
        const QString t = tool_->toolTipAt(he->pos());
        if (t.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(he->globalPos(), t, this);
        return true;
    }
    return QOpenGLWidget::event(e);
}

void Viewport::leaveEvent(QEvent* e) {
    if (tool_) tool_->leave();
    setHover({});
    QOpenGLWidget::leaveEvent(e);
}

}  // namespace construe::app
