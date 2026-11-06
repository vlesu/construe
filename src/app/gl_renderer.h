// Drawing the 3D view with OpenGL 3.3 core / 3.0 (docs/dev/client.md §4): shaders, buffers, body meshes on the GPU,
// background, bodies with edges, origin planes, sketch regions, the pick ID-buffer pass. The view (Viewport) passes
// the scene description and gets the ID frame, depth and pick table; camera, input and picking stay with the view.
// All methods are called with the view's OpenGL context current.
#pragma once

#include "app/camera.h"
#include "app/captions.h"
#include "app/display_style.h"
#include "app/viewport_types.h"

#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace construe::app {

// What to draw in the frame.
struct RenderScene {
    const Camera& camera;
    const std::vector<DrawBody>& bodies;
    bool planesVisible;
    double planeHalf;
    const std::vector<DrawRegion>& regions;
    bool regionsOnTop;
    const PickTarget& hover;
    const std::set<PickTarget>& selection;
    const std::set<std::string>& shownPlanes;  // with bodies — the visible planes (hover in the tree, eye, §3.4)
    const std::vector<DrawPlane>& planes;      // origin and reference planes
    bool allPlanesShown;                       // reference geometry panel: all planes visible (§3.4)
    DisplayStyle style = DisplayStyle::ShadedEdges;  // display style (§3.4а)
    const std::set<PickTarget>* hoverSet = nullptr;   // geometry of the feature hovered in the tree (§4.4, FX7-PT-011)
    bool hoverEmphasis = false;                       // hoverSet is a hovered selection list row (§5.5а, FX10-WZ-002)
    // Device pixels per logical pixel of the view (HiDPI, macOS Retina — 2): the widths of edges and dashes of §11.4
    // are logical pixels, the shaders work in device pixels (client.md §4.3а).
    float pixelRatio = 1.0f;
    bool hovered(const PickTarget& t) const { return hover == t || (hoverSet && hoverSet->count(t)); }
    // A hovered selection list row: bright orange over the selection.
    bool emphasized(const PickTarget& t) const { return hoverEmphasis && hoverSet && hoverSet->count(t); }

    // A plane is drawn and pickable: without bodies — always; with bodies — if hovered in the tree, pinned or selected.
    bool planeShown(const std::string& id) const {
        if (!planesVisible) return false;
        if (allPlanesShown || bodies.empty() || shownPlanes.count(id)) return true;
        return selection.count(PickTarget{PickTarget::Kind::Plane, {}, id}) > 0;
    }
};

// Draw-call batch (FX3-PERF-003, docs/dev/client.md §4): consecutive body elements (faces or edges) with
// the same style and adjacent buffer ranges are drawn in one call. first/count — buffer range, style — key.
struct DrawRun {
    int first = 0, count = 0;
    int style = 0;
};
// ranges[i] — range of element i (first, count) in the buffer; styleOf(i) — style key, < 0 — element is not drawn.
// Element order is preserved: drawing the batches gives the same result as drawing elements one by one.
std::vector<DrawRun> drawRuns(const std::vector<std::pair<int, int>>& ranges, const std::function<int(size_t)>& styleOf);

// ID pass result: pixel color — ID = index in table + 1 (24-bit RGB), depth [0, 1], rows bottom to top.
struct IdFrame {
    QImage image;
    std::vector<float> depth;
    std::vector<PickTarget> table;
};

class GlRenderer : protected QOpenGLExtraFunctions {
public:
    // core33 — an OpenGL 3.3 core context (otherwise GLSL 1.30 of Mesa software rendering).
    void initialize(bool core33);
    void release();  // release GL resources
    bool ready() const { return ready_; }
    // Body meshes go to the GPU; bodies with the same key reuse buffers.
    void uploadBodies(const std::vector<DrawBody>& bodies);
    // ID pass into an off-screen buffer of size sz (physical px).
    IdFrame renderIdPass(const RenderScene& s, const QSize& sz);
    // Frame into buffer fbo of size sz: background, bodies, planes, regions.
    void renderColor(const RenderScene& s, const QSize& sz, GLuint fbo);
    void resetIdBuffer() { idFbo_.reset(); }
    // Draw calls (glDraw*) in the last "color" pass (test mode, FX3-PERF-003).
    int lastColorDrawCalls() const { return lastColorDraws_; }

private:
    struct GpuBody {
        std::string key;
        QOpenGLVertexArrayObject vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        QOpenGLVertexArrayObject edgeVao;
        QOpenGLBuffer edgeVbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLVertexArrayObject wideVao;  // wide edges: 6 vertices per segment of edgeVbo (FX7-PT-001)
        QOpenGLBuffer wideVbo{QOpenGLBuffer::VertexBuffer};
        int indexCount = 0;
        std::vector<std::pair<int, int>> faceRanges;  // (first index, count)
        std::vector<std::pair<int, int>> edgeRanges;  // (first vertex, count) — GL_LINES
        int edgeVertexCount = 0;
    };
    void drawBackground();
    void drawBodies(const RenderScene& s, bool idPass);
    void drawBodyEdges(const RenderScene& s, const DrawBody& b, GpuBody& g, bool idPass);
    void drawHiddenEdges(const RenderScene& s, const QSize& sz);  // dashes of hidden edges (§3.4а)
    void drawPlanes(const RenderScene& s, bool idPass);
    void drawRegions(const RenderScene& s, bool idPass);
    uint32_t pickId(const PickTarget& t);
    // n consecutive pick numbers (body elements in order); returns the first.
    uint32_t pickBlock(const std::vector<PickTarget>& targets);
    void drawElements(GLenum mode, int first, int count);  // glDrawElements with a counter
    void drawArrays(GLenum mode, int first, int count);    // glDrawArrays with a counter
    void setWideUniforms(QOpenGLShaderProgram& prog, const RenderScene& s);  // mvp and viewport of wide edges

    QOpenGLShaderProgram meshProg_, flatProg_, bgProg_, dashProg_, idProg_;
    QOpenGLShaderProgram wideProg_, wideIdProg_;  // wide edges: color and ID pass (FX7-PT-001)
    bool wideOk_ = false;
    QOpenGLVertexArrayObject bgVao_, dynVao_;
    QOpenGLBuffer dynVbo_{QOpenGLBuffer::VertexBuffer};
    std::unique_ptr<QOpenGLFramebufferObject> idFbo_;
    std::vector<std::unique_ptr<GpuBody>> gpu_;
    std::vector<PickTarget> pickTable_;
    bool ready_ = false;
    int draws_ = 0, lastColorDraws_ = 0;
};

// ID color for the pick pass.
QVector4D idColor(uint32_t id);

}  // namespace construe::app
