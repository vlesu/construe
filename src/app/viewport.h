// 3D view: camera and navigation, picking via the ID buffer and geometrically, labels over the frame; mesh drawing —
// GlRenderer (docs/dev/client.md §4, spec/app-ux.md §3).
#pragma once

#include "app/camera.h"
#include "app/gl_renderer.h"
#include "app/viewport_types.h"
#include "app/theme.h"
#include "io/stl.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::app {

// Input handler on top of navigation (sketcher, selection in a part).
class ViewportTool {
public:
    virtual ~ViewportTool() = default;
    virtual bool mousePress(QMouseEvent*) { return false; }
    virtual bool mouseMove(QMouseEvent*) { return false; }
    virtual bool mouseRelease(QMouseEvent*) { return false; }
    virtual bool mouseDoubleClick(QMouseEvent*) { return false; }
    virtual bool keyPress(QKeyEvent*) { return false; }
    virtual void paint(QPainter&) {}
    // Tooltip text at a view point (app-ux §3.5, §6.6); empty — no tooltip.
    virtual QString toolTipAt(const QPoint&) const { return {}; }
    virtual void leave() {}
};

// GL_RENDERER string of software OpenGL (Mesa llvmpipe/softpipe/swrast, GDI Generic on Windows, Microsoft Basic Render
// Driver) — app-ux §2.1 «About», docs/dev/client.md §2а.
bool isSoftwareGlRenderer(const QString& renderer);
// The {gl} text of «About» (app-ux §2.1): OpenGL kind by the GL_RENDERER string; autoSoftwareVersion —
// the graphics card GL_VERSION on automatic fallback to software OpenGL (FX3-PERF-004), empty — no fallback happened.
QString aboutGlText(const QString& renderer, const QString& autoSoftwareVersion);

class Viewport : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }
    void setCamera(const Camera& c);
    // Smooth transition to the camera (app-ux §3.3) over animationMs(); 0 — instant. Input in the view interrupts it.
    void animateTo(const Camera& target);
    void stopAnimation();
    bool isAnimating() const { return animTimer_ != nullptr && animTimer_->isActive(); }
    // Target of the running transition, otherwise the current camera: the next standard views are computed from it.
    Camera targetCamera() const;
    void setAnimationMs(int ms) { animMs_ = ms; }
    int animationMs() const { return animMs_; }

    void setBodies(std::vector<DrawBody> bodies);
    const std::vector<DrawBody>& bodies() const { return bodies_; }
    void setPlanesVisible(bool v);
    // Origin planes shown when bodies exist (hover in the tree, app-ux §3.4, S1-VIEW-006).
    void setShownPlanes(const std::set<std::string>& ids);
    // Geometry of the feature hovered in the tree or of a row hovered in a selection list, drawn as hovered (§4.4,
    // FX7-PT-011; §5.5а, S7F-UI-001).
    // emphasis — a selection list row (FX10-WZ-002): drawn bright orange COLOR_LIST_HOVER over the selection color.
    void setTreeHover(const std::set<PickTarget>& targets, bool emphasis = false);
    const std::set<PickTarget>& treeHover() const { return treeHover_; }
    bool treeHoverEmphasis() const { return treeHoverEmphasis_; }
    // Drawn as hovered: under the cursor or in treeHover().
    bool hovered(const PickTarget& t) const { return hover_ == t || treeHover_.count(t) > 0; }
    // Drawn as a hovered selection list row (over the selection).
    bool emphasized(const PickTarget& t) const { return treeHoverEmphasis_ && treeHover_.count(t) > 0; }
    // Bounding box of view objects (faces, edges, vertices, planes, axes, regions, sketch elements, welds); false —
    // none of them is in the view.
    bool targetBounds(const std::set<PickTarget>& targets, QVector3D& mn, QVector3D& mx) const;
    // The view is centered on the objects so that they take SELECTION_FRAME_FRACTION of it (§5.5а, S7F-UI-001).
    bool centerOn(const std::set<PickTarget>& targets, bool animated = true);
    // Reference planes and axes of the shown state (app-ux §3.4, S2-REF-*); allShown — the reference geometry panel.
    void setReferenceGeometry(std::vector<DrawPlane> planes, std::vector<DrawAxis> axes, bool allShown);
    const std::vector<DrawPlane>& planes() const { return planes_; }
    const std::vector<DrawAxis>& refAxes() const { return refAxes_; }
    // Assembly weld lines (app-ux §8а, S5-WELD-002) — data only, not drawn (S7F-WLD-001: the weld is its red body);
    // pickable — a click on a weld body picks the weld (Part mode).
    void setWelds(std::vector<DrawWeld> welds, bool pickable);
    const std::vector<DrawWeld>& welds() const { return welds_; }
    // Planes, axes and sketches of nested subassemblies (FX9-ASM-008): drawn only; planes by the rule of own planes.
    void setNestedReference(DrawNestedReference n);
    const DrawNestedReference& nestedReference() const { return nested_; }
    bool nestedPlaneShown(const std::string& id) const;
    // Nested planes and axes pickable (mate mode, FX9-ASM-014): a pick is Plane / Axis with body = the carrying node
    // path and name = the feature ID.
    void setNestedPickable(bool on);
    bool planeShown(const std::string& id) const { return renderScene().planeShown(id); }
    bool planesVisible() const { return planesVisible_; }
    void setPlaneHalf(double h);
    double planeHalf() const { return planeHalf_; }
    void setTool(ViewportTool* t) { tool_ = t; update(); }

    // Highlight/selection of ID-buffer objects.
    void setHover(const PickTarget& t);
    void setSelection(const std::set<PickTarget>& s);
    const PickTarget& hover() const { return hover_; }
    const std::set<PickTarget>& selection() const { return selection_; }

    // Picking by the §3.5 rules (edges before faces) within the PICK_WINDOW_PX window.
    // Vertices are picked together with edges (edgesAllowed), the origin and axes — with faces (facesAllowed).
    PickTarget pickAt(const QPoint& p, bool facesAllowed = true, bool edgesAllowed = true, bool planesAllowed = true) const;
    // Reference axis feature (not a datum axis axis:*) within PICK_CURVE_PX of p — a sketch base (app-ux §6.9, FX9-SK-001).
    std::string refAxisAt(const QPoint& p) const;
    // Origin axes: hidden until needed (app-ux §3.4); a selected/hovered axis is always visible.
    void setAxesVisible(bool v);
    // Mouse profile (app-ux §3.1) and wheel zoom inversion (§3.2).
    // MiddleButton — rotation with the middle button (default); RightButton — rotation with the right button.
    enum class MouseProfile { MiddleButton, RightButton };
    void setMouseProfile(MouseProfile p) { mouseProfile_ = p; }
    MouseProfile mouseProfile() const { return mouseProfile_; }
    void setInvertWheel(bool on) { invertWheel_ = on; }
    // Display style (app-ux §3.4а).
    void setDisplayStyle(DisplayStyle s) {
        style_ = s;
        ++sceneGen_;
        update();
    }
    DisplayStyle displayStyle() const { return style_; }
    bool invertWheel() const { return invertWheel_; }
    // Visible sketch regions; onTop — «Profiles» field active: regions pickable over bodies (§3.5, prio 4).
    void setRegions(std::vector<DrawRegion> regions, bool onTop);
    // Pickable elements of visible sketches (app-ux §3.4, FX7-SEL-001): points with vertices, lines and curves with
    // edges (§3.5 priorities 2 and 3); empty — not pickable.
    void setSketchPicks(std::vector<DrawSketchPick> picks);
    const std::vector<DrawSketchPick>& sketchPicks() const { return sketchPicks_; }
    const std::vector<DrawRegion>& regions() const { return regions_; }
    bool axesVisible() const;
    // Depth under the cursor (from the Z buffer of the ID pass) or nullopt.
    std::optional<QVector3D> pointUnder(const QPoint& p) const;

    // Test mode (§12.1).
    std::optional<QPoint> screenPosOf(const PickTarget& t) const;
    bool frameUpToDate() const { return !dirty_; }
    // GL_RENDERER of the view's context; empty — the view is not initialized yet.
    const QString& glRenderer() const { return glRenderer_; }
    uint64_t frameNumber() const { return frame_; }
    // OpenGL draw calls in the last frame (FX3-PERF-003: body edges and faces — in few calls).
    int lastFrameDrawCalls() const { return renderer_.lastColorDrawCalls(); }

    // Bounding box of the visible geometry (for «Fit» and the rotation center).
    bool sceneBounds(QVector3D& mn, QVector3D& mx) const;
    void fitAll(bool animated = false);
    // Camera c fitted to the visible geometry (like «Fit», §3.3): the direction of c is preserved.
    Camera fitted(Camera c) const;

    // A world point is not hidden by opaque bodies (ray to the camera, Möller–Trumbore); used for vertex picking and
    // annotation occlusion (FX12-PMI-001).
    bool pointVisible(const QVector3D& w) const;

signals:
    void cameraChanged();
    void selectionChanged();  // the selection changed (command availability, app-ux §7.5)
    void framePainted();
    void bodiesSet();  // a new scene was handed to the view (each shown frame of a drag, S7F-ASM-001)

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent*) override;

private:
    void animationStep();
    QTimer* animTimer_ = nullptr;
    QElapsedTimer animClock_;
    Camera animFrom_, animTo_;
    int animMs_ = ANIM_VIEW_MS;

    void drawDatumsAndVertices(QPainter& p);
    void drawNestedReference(QPainter& p);
    // Geometric picking: nearest visible body vertex; origin; axis. d — screen distance.
    std::optional<std::pair<double, PickTarget>> pickVertex(const QPointF& s) const;
    std::optional<PickTarget> pickDatum(const QPointF& s) const;
    // Sketch element: point within PICK_POINT_PX (curves=false) or a line/curve within PICK_CURVE_PX (curves=true).
    std::optional<std::pair<double, PickTarget>> pickSketch(const QPointF& s, bool curves) const;
    std::pair<QPointF, QPointF> axisSegment(int i) const;
    std::optional<std::pair<QPointF, QPointF>> axisSegmentOf(const std::string& id) const;  // origin or reference
    // Nested plane / axis of a pick target (body — node path, name — feature ID; FX9-ASM-014).
    const DrawPlane* nestedPlaneOf(const PickTarget& t) const;
    const DrawAxis* nestedAxisOf(const PickTarget& t) const;
    QPolygonF nestedPlaneQuad(const DrawPlane& pd) const;
    void drawOrigin(QPainter& p);
    RenderScene renderScene() const;
    // ID buffer on demand (app-ux §3.5, docs/dev/client.md §4): rebuilt on the first pick after a change of the
    // scene, camera or size, not every frame.
    void ensureIdBuffer() const;
    QVector3D rotationPivot(const QPoint& p) const;
    void updateDepthRange();

    Camera camera_;
    std::vector<DrawBody> bodies_;
    bool bodiesDirty_ = true;
    bool planesVisible_ = true;
    std::set<std::string> shownPlanes_;
    std::set<PickTarget> treeHover_;
    bool treeHoverEmphasis_ = false;
    std::vector<DrawPlane> planes_;    // origin + reference (§3.4)
    std::vector<DrawAxis> refAxes_;
    std::vector<DrawWeld> welds_;
    DrawNestedReference nested_;
    bool nestedPickable_ = false;
    bool weldsPickable_ = false;
    bool allPlanesShown_ = false;
    bool axesVisible_ = false;
    std::vector<DrawRegion> regions_;
    std::vector<DrawSketchPick> sketchPicks_;
    bool regionsOnTop_ = false;
    double planeHalf_ = PLANE_MIN_HALF_MM;
    ViewportTool* tool_ = nullptr;
    PickTarget hover_;
    std::set<PickTarget> selection_;

    GlRenderer renderer_;
    mutable QImage idImage_;
    mutable std::vector<float> depth_;
    mutable std::vector<PickTarget> pickTable_;  // index = ID − 1
    uint64_t sceneGen_ = 1;          // changes when the scene contents change (not hover and selection)
    mutable uint64_t idGen_ = 0;     // sceneGen_ of the ID buffer
    mutable QMatrix4x4 idViewProj_;  // camera of the ID buffer
    mutable QSize idSize_;
    bool inPaint_ = false;
    QString glRenderer_;
    bool dirty_ = true;
    uint64_t frame_ = 0;

    // Navigation (§3.1).
    enum class Gesture { None, Rotate, Pan, Zoom };
    Gesture gesture_ = Gesture::None;
    bool gestureArmed_ = false;
    Qt::MouseButton gestureButton_ = Qt::NoButton;
    Qt::KeyboardModifiers gestureMods_;
    MouseProfile mouseProfile_ = MouseProfile::MiddleButton;
    bool invertWheel_ = false;
    DisplayStyle style_ = DisplayStyle::ShadedEdges;
    std::optional<QPointF> rightPress_;  // RightButton profile: RMB without dragging — a regular click (context menu)
    QPoint pressPos_;
    QPoint lastPos_;
    Camera cam0_;
    QVector3D pivot_;
};

}  // namespace construe::app
