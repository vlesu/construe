// Drawing the 3D view with OpenGL (app/gl_renderer.h).
#include "app/gl_renderer.h"

#include "api/session.h"
#include "app/theme.h"
#include "base/log.h"

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {

const char* MESH_VS = R"(in vec3 pos;
in vec3 nrm;
uniform mat4 mvp;
uniform vec3 viewDir;
uniform float ambient;
out float shade;
void main() {
    gl_Position = mvp * vec4(pos, 1.0);
    shade = ambient + (1.0 - ambient) * abs(dot(normalize(nrm), -viewDir));
}
)";
const char* MESH_FS = R"(in float shade;
uniform vec4 color;
out vec4 frag;
void main() { frag = vec4(color.rgb * shade, color.a); }
)";
// Wide edges of the color pass are moved towards the viewer by this much of clip w (FX7-PT-001): the quad overhangs
// its line by width/2 pixels onto the adjacent faces and on Windows software OpenGL lost the depth test there, so the
// selected edge looked thin. The ID pass keeps 0 — picking of edges is unchanged.
constexpr float WIDE_EDGE_DEPTH_BIAS = 5e-5f;
constexpr int WIDE_FLOATS = 9;  // floats of a wide edge vertex: pos, other, corner, pid

const char* FLAT_VS = R"(in vec3 pos;
uniform mat4 mvp;
void main() { gl_Position = mvp * vec4(pos, 1.0); }
)";
// Wide lines (FX7-PT-001) without glLineWidth and without geometry shaders, the same on every OpenGL 3.0+ context and
// driver: each GL_LINES segment a–b is 6 vertices of two triangles (the edge buffer of the body, GpuBody::wideVbo);
// a vertex carries its end, the other end and the corner (x: 0 — at a, 1 — at b; y: side ±1). The vertex shader
// builds a screen-space quad of `width` pixels, extended by half the width at both ends (polyline joints stay closed).
const char* WIDE_FN = R"(in vec3 pos;
in vec3 other;
in vec2 corner;
uniform mat4 mvp;
uniform vec2 viewport;
uniform float width;
uniform float depthBias;
vec4 widePosition() {
    vec4 p = mvp * vec4(pos, 1.0), o = mvp * vec4(other, 1.0);
    vec2 hv = 0.5 * viewport;
    vec2 sp = p.xy / p.w * hv, so = o.xy / o.w * hv;
    vec2 d = corner.x < 0.5 ? so - sp : sp - so;
    float l = length(d);
    vec2 t = l > 1e-6 ? d / l : vec2(1.0, 0.0);
    vec2 n = vec2(-t.y, t.x) * (0.5 * width), e = t * (0.5 * width);
    vec2 s = sp + (corner.x < 0.5 ? -e : e) + corner.y * n;
    return vec4(s / hv * p.w, p.z - depthBias * p.w, p.w);
}
)";
const char* WIDE_VS = R"(void main() { gl_Position = widePosition(); }
)";
// The ID pass of edges (FX3-PERF-003) — the same quads, the pick number as in ID_VS.
const char* WIDE_ID_VS = R"(in float pid;
uniform float base;
flat out float id;
void main() {
    gl_Position = widePosition();
    id = pid < 0.5 ? -1.0 : base + pid - 1.0;
}
)";
const char* FLAT_FS = R"(uniform vec4 color;
out vec4 frag;
void main() { frag = color; }
)";
// Dashes in screen pixels from the segment end (the provoking vertex of GL_LINES is the second one).
const char* DASH_VS = R"(in vec3 pos;
uniform mat4 mvp;
uniform vec2 viewport;
noperspective out vec2 px;
flat out vec2 startPx;
void main() {
    vec4 p = mvp * vec4(pos, 1.0);
    gl_Position = p;
    vec2 s = (p.xy / p.w * 0.5 + 0.5) * viewport;
    px = s;
    startPx = s;
}
)";
const char* DASH_FS = R"(noperspective in vec2 px;
flat in vec2 startPx;
uniform vec4 color;
uniform vec2 dash;
out vec4 frag;
void main() {
    if (mod(length(px - startPx), dash.x + dash.y) > dash.x) discard;
    frag = color;
}
)";
// ID pass with one call per body (FX3-PERF-003): the body element index (face or edge) + 1 is the vertex attribute pid
// (0 — an unnamed element, not pickable); the pick number = base + pid − 1 is encoded in RGB (as idColor).
const char* ID_VS = R"(in vec3 pos;
in float pid;
uniform mat4 mvp;
uniform float base;
flat out float id;
void main() {
    gl_Position = mvp * vec4(pos, 1.0);
    id = pid < 0.5 ? -1.0 : base + pid - 1.0;
}
)";
const char* ID_FS = R"(flat in float id;
out vec4 frag;
void main() {
    if (id < 0.0) discard;
    frag = vec4(mod(id, 256.0), mod(floor(id / 256.0), 256.0), floor(id / 65536.0), 255.0) / 255.0;
}
)";
const char* BG_VS = R"(out float t;
void main() {
    vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID & 2) * 2.0 - 1.0);
    t = p.y * 0.5 + 0.5;
    gl_Position = vec4(p, 0.999, 1.0);
}
)";
const char* BG_FS = R"(in float t;
uniform vec4 top;
uniform vec4 bottom;
out vec4 frag;
void main() { frag = mix(bottom, top, clamp(t, 0.0, 1.0)); }
)";

QVector4D rgba(const QColor& c, float a = 1.0f) { return QVector4D(float(c.redF()), float(c.greenF()), float(c.blueF()), a); }

QVector3D q(const Vec3& v) { return QVector3D(float(v.x()), float(v.y()), float(v.z())); }

// Blend the body color with the highlight color.
QColor mix(const QColor& base, const QColor& over, double a) {
    return QColor::fromRgbF(base.redF() * (1 - a) + over.redF() * a, base.greenF() * (1 - a) + over.greenF() * a,
                            base.blueF() * (1 - a) + over.blueF() * a);
}

}  // namespace

std::vector<DrawRun> drawRuns(const std::vector<std::pair<int, int>>& ranges, const std::function<int(size_t)>& styleOf) {
    std::vector<DrawRun> out;
    bool open = false;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const int st = styleOf(i);
        if (st < 0) {
            open = false;
            continue;
        }
        const auto [first, count] = ranges[i];
        if (open && out.back().style == st && out.back().first + out.back().count == first) {
            out.back().count += count;
            continue;
        }
        out.push_back({first, count, st});
        open = true;
    }
    return out;
}

QVector4D idColor(uint32_t id) {
    return QVector4D(float(id & 0xFF) / 255.f, float((id >> 8) & 0xFF) / 255.f, float((id >> 16) & 0xFF) / 255.f, 1.f);
}

void GlRenderer::initialize(bool core33) {
    initializeOpenGLFunctions();
    // OpenGL 3.3 core (normal mode) or 3.0 (Mesa software rendering): GLSL version by context.
    const QByteArray header = core33 ? "#version 330 core\n" : "#version 130\n";
    auto build = [&](QOpenGLShaderProgram& prog, const char* vs, const char* fs, bool normals, bool pid = false) {
        prog.addShaderFromSourceCode(QOpenGLShader::Vertex, header + vs);
        prog.addShaderFromSourceCode(QOpenGLShader::Fragment, header + fs);
        prog.bindAttributeLocation("pos", 0);
        if (normals) prog.bindAttributeLocation("nrm", 1);
        if (pid) prog.bindAttributeLocation("pid", 2);
        if (!prog.link()) log::warn(("OpenGL: shader link failed: " + prog.log()).toStdString());
    };
    build(meshProg_, MESH_VS, MESH_FS, true);
    build(flatProg_, FLAT_VS, FLAT_FS, false);
    build(bgProg_, BG_VS, BG_FS, false);
    build(dashProg_, DASH_VS, DASH_FS, false);
    build(idProg_, ID_VS, ID_FS, false, true);
    // Wide edges (FX7-PT-001): quads built by the vertex shader — one path for every context (no geometry shaders).
    auto buildWide = [&](QOpenGLShaderProgram& prog, const char* vs, const char* fs) {
        prog.addShaderFromSourceCode(QOpenGLShader::Vertex, header + WIDE_FN + vs);
        prog.addShaderFromSourceCode(QOpenGLShader::Fragment, header + fs);
        prog.bindAttributeLocation("pos", 0);
        prog.bindAttributeLocation("pid", 2);
        prog.bindAttributeLocation("other", 3);
        prog.bindAttributeLocation("corner", 4);
        const bool ok = prog.link();
        if (!ok) log::warn(("OpenGL: wide line shader link failed: " + prog.log()).toStdString());
        return ok;
    };
    wideOk_ = buildWide(wideProg_, WIDE_VS, FLAT_FS);
    wideOk_ = buildWide(wideIdProg_, WIDE_ID_VS, ID_FS) && wideOk_;
    // Diagnostics: does glLineWidth > 1 work in this context (forward-compatible core profiles refuse it)?
    while (glGetError() != GL_NO_ERROR) {}
    glLineWidth(3.0f);
    GLfloat lw = 0;
    glGetFloatv(GL_LINE_WIDTH, &lw);
    const bool lwError = glGetError() != GL_NO_ERROR;
    glLineWidth(1.0f);
    while (glGetError() != GL_NO_ERROR) {}
    log::info(QString("OpenGL: glLineWidth(3) → %1%2").arg(double(lw)).arg(lwError ? " (error)" : "").toStdString());
    // Without the wide edge shaders selected edges are thin — a warning, so that the cause is visible in the log.
    (wideOk_ ? log::info : log::warn)(QString("OpenGL: shaders GLSL %1, wide edges — %2")
                  .arg(core33 ? "3.30 core" : "1.30", wideOk_ ? "vertex shader quads" : "unavailable (1 px lines)")
                  .toStdString());
    bgVao_.create();
    dynVao_.create();
    dynVbo_.create();
    dynVbo_.setUsagePattern(QOpenGLBuffer::StreamDraw);
    ready_ = true;
}

void GlRenderer::release() {
    gpu_.clear();
    idFbo_.reset();
    if (ready_) {
        dynVbo_.destroy();
        dynVao_.destroy();
        bgVao_.destroy();
        meshProg_.removeAllShaders();
        flatProg_.removeAllShaders();
        bgProg_.removeAllShaders();
        dashProg_.removeAllShaders();
        idProg_.removeAllShaders();
        wideProg_.removeAllShaders();
        wideIdProg_.removeAllShaders();
    }
    wideOk_ = false;
    ready_ = false;
}

void GlRenderer::uploadBodies(const std::vector<DrawBody>& bodies) {
    std::vector<std::unique_ptr<GpuBody>> next;
    for (const auto& b : bodies) {
        auto it = std::find_if(gpu_.begin(), gpu_.end(), [&](const auto& g) { return g && g->key == b.key; });
        if (it != gpu_.end()) {
            next.push_back(std::move(*it));
            continue;
        }
        auto g = std::make_unique<GpuBody>();
        g->key = b.key;
        const auto& m = b.mesh;
        // Vertex face index + 1 (0 — unnamed face) for the single-call ID pass (FX3-PERF-003); face nodes are not
        // shared (core-api §5.2), so a vertex has one face.
        const size_t nv = m.positions.size() / 3;
        std::vector<float> pid(nv, 0.f);
        for (size_t fi = 0; fi < m.faceRanges.size(); ++fi) {
            if (m.faceRanges[fi].name.empty()) continue;
            const size_t t0 = size_t(m.faceRanges[fi].firstTriangle) * 3, t1 = t0 + size_t(m.faceRanges[fi].count) * 3;
            for (size_t t = t0; t < t1 && t < m.triangles.size(); ++t)
                if (m.triangles[t] < nv) pid[m.triangles[t]] = float(fi + 1);
        }
        std::vector<float> v;
        v.reserve(nv * 7);
        for (size_t i = 0; i < nv; ++i) {
            for (int k = 0; k < 3; ++k) v.push_back(m.positions[3 * i + k]);
            for (int k = 0; k < 3; ++k) v.push_back(i * 3 + k < m.normals.size() ? m.normals[3 * i + k] : 0.f);
            v.push_back(pid[i]);
        }
        g->vao.create();
        g->vao.bind();
        g->vbo.create();
        g->vbo.bind();
        g->vbo.allocate(v.data(), int(v.size() * sizeof(float)));
        g->ibo.create();
        g->ibo.bind();
        g->ibo.allocate(m.triangles.data(), int(m.triangles.size() * sizeof(uint32_t)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
        g->vao.release();
        g->indexCount = int(m.triangles.size());
        for (const auto& fr : m.faceRanges) g->faceRanges.push_back({int(fr.firstTriangle * 3), int(fr.count * 3)});
        std::vector<float> e;  // x, y, z, edge index + 1 (0 — unnamed)
        for (size_t ei = 0; ei < m.edges.size(); ++ei) {
            const auto& edge = m.edges[ei];
            const float id = edge.name.empty() ? 0.f : float(ei + 1);
            const int first = int(e.size() / 4);
            for (const auto& pl : edge.polylines)
                for (size_t i = 0; i + 5 < pl.size(); i += 3)
                    for (int j = 0; j < 2; ++j) {
                        for (int k = 0; k < 3; ++k) e.push_back(pl[i + 3 * j + k]);
                        e.push_back(id);
                    }
            g->edgeRanges.push_back({first, int(e.size() / 4) - first});
        }
        g->edgeVertexCount = int(e.size() / 4);
        // Wide edges (FX7-PT-001): segment i of the line buffer (vertices 2i, 2i + 1) is vertices 6i … 6i + 5 here.
        std::vector<float> wv;  // pos, other, corner, edge index + 1
        wv.reserve(size_t(g->edgeVertexCount) * 3 * WIDE_FLOATS);
        for (size_t i = 0; i + 1 < size_t(g->edgeVertexCount); i += 2) {
            const float* a = &e[4 * i];
            const float* b = &e[4 * (i + 1)];
            auto vert = [&](const float* p, const float* o, float atEnd, float side) {
                wv.insert(wv.end(), {p[0], p[1], p[2], o[0], o[1], o[2], atEnd, side, p[3]});
            };
            vert(a, b, 0, 1), vert(a, b, 0, -1), vert(b, a, 1, 1);
            vert(a, b, 0, -1), vert(b, a, 1, -1), vert(b, a, 1, 1);
        }

        g->edgeVao.create();
        g->edgeVao.bind();
        g->edgeVbo.create();
        g->edgeVbo.bind();
        g->edgeVbo.allocate(e.data(), int(e.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
        g->edgeVao.release();
        g->wideVao.create();
        g->wideVao.bind();
        g->wideVbo.create();
        g->wideVbo.bind();
        g->wideVbo.allocate(wv.data(), int(wv.size() * sizeof(float)));
        const int stride = WIDE_FLOATS * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8 * sizeof(float)));
        g->wideVao.release();
        next.push_back(std::move(g));
    }
    gpu_ = std::move(next);
}

uint32_t GlRenderer::pickId(const PickTarget& t) {
    pickTable_.push_back(t);
    return static_cast<uint32_t>(pickTable_.size());
}

uint32_t GlRenderer::pickBlock(const std::vector<PickTarget>& targets) {
    const auto first = static_cast<uint32_t>(pickTable_.size() + 1);
    pickTable_.insert(pickTable_.end(), targets.begin(), targets.end());
    return first;
}

void GlRenderer::drawElements(GLenum mode, int first, int count) {
    ++draws_;
    glDrawElements(mode, count, GL_UNSIGNED_INT, reinterpret_cast<void*>(size_t(first) * sizeof(uint32_t)));
}

void GlRenderer::drawArrays(GLenum mode, int first, int count) {
    ++draws_;
    glDrawArrays(mode, first, count);
}

void GlRenderer::drawBackground() {
    glDisable(GL_DEPTH_TEST);
    bgProg_.bind();
    bgProg_.setUniformValue("top", rgba(COLOR_BG_TOP.color));
    bgProg_.setUniformValue("bottom", rgba(COLOR_BG_BOTTOM.color));
    bgVao_.bind();
    drawArrays(GL_TRIANGLES, 0, 3);
    bgVao_.release();
    glEnable(GL_DEPTH_TEST);
}

void GlRenderer::drawRegions(const RenderScene& s, bool idPass) {
    if (s.regions.empty()) return;
    flatProg_.bind();
    flatProg_.setUniformValue("mvp", s.camera.viewProjection());
    dynVao_.bind();
    dynVbo_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    // ID pass: with the «Profiles» field active — over everything, otherwise at the depth of coincident
    // faces: the face (drawn earlier) stays, a coincident origin plane loses (§3.5: face 5, region 4 when
    // "not over a body", plane 6). The hover/selection fill goes in front of the coincident face to be visible.
    if (idPass && s.regionsOnTop) glDisable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    if (idPass && !s.regionsOnTop) glPolygonOffset(1.0f, 1.0f);  // as for faces: the face (earlier in pass) wins, the plane (2, 4) does not
    else glPolygonOffset(-1.0f, -2.0f);
    if (!idPass) glDepthMask(GL_FALSE);
    for (const auto& r : s.regions) {
        const PickTarget t{PickTarget::Kind::Region, r.sketch, r.regionId};
        QVector4D color;
        if (idPass) color = idColor(pickId(t));
        else if (s.emphasized(t)) color = rgba(COLOR_LIST_HOVER.color, float(LIST_HOVER_ALPHA));
        else if (s.selection.count(t)) color = rgba(COLOR_SELECTED.color, float(FACE_SELECTED_ALPHA));
        else if (s.hovered(t)) color = rgba(COLOR_HOVER.color, float(REGION_HOVER_ALPHA));
        else continue;  // unselected regions are not filled
        std::vector<float> buf;
        buf.reserve(r.triangles.size() * 3);
        for (const auto& v : r.triangles) buf.insert(buf.end(), {v.x(), v.y(), v.z()});
        flatProg_.setUniformValue("color", color);
        dynVbo_.allocate(buf.data(), int(buf.size() * sizeof(float)));
        drawArrays(GL_TRIANGLES, 0, int(r.triangles.size()));
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_DEPTH_TEST);
    dynVao_.release();
}

void GlRenderer::drawPlanes(const RenderScene& s, bool idPass) {
    if (!s.planesVisible) return;
    flatProg_.bind();
    flatProg_.setUniformValue("mvp", s.camera.viewProjection());
    dynVao_.bind();
    dynVbo_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    for (const auto& pd : s.planes) {
        if (!s.planeShown(pd.id)) continue;
        const auto& pl = pd.frame;
        const QVector3D x = q(pl.x) * float(s.planeHalf), y = q(pl.y) * float(s.planeHalf);
        const QVector3D c[4] = {pd.center - x - y, pd.center + x - y, pd.center + x + y, pd.center - x + y};
        float quad[18];
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i)
            for (int k = 0; k < 3; ++k) quad[3 * i + k] = c[order[i]][k];
        float border[24];
        for (int i = 0; i < 4; ++i) {
            for (int k = 0; k < 3; ++k) border[6 * i + k] = c[i][k];
            for (int k = 0; k < 3; ++k) border[6 * i + 3 + k] = c[(i + 1) % 4][k];
        }
        const PickTarget t{PickTarget::Kind::Plane, {}, pd.id};
        QVector4D fill = rgba(COLOR_PLANE_FILL.color, float(PLANE_FILL_OPACITY));
        QVector4D line = rgba(COLOR_PLANE_BORDER.color);
        if (s.emphasized(t)) fill = rgba(COLOR_LIST_HOVER.color, float(LIST_HOVER_ALPHA)), line = rgba(COLOR_LIST_HOVER.color);
        else if (s.selection.count(t)) fill = rgba(COLOR_SELECTED.color, float(FACE_SELECTED_ALPHA)), line = rgba(COLOR_SELECTED.color);
        else if (s.hovered(t)) fill = rgba(COLOR_HOVER.color, float(FACE_HOVER_ALPHA)), line = rgba(COLOR_HOVER.color);
        if (idPass) fill = line = idColor(pickId(t));
        glDepthMask(idPass && s.bodies.empty() ? GL_TRUE : GL_FALSE);
        // The plane is farther than a coincident body face (faces have offset 1, 1): in the ID buffer and on screen
        // the face wins, not z-fighting.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f);
        flatProg_.setUniformValue("color", fill);
        dynVbo_.allocate(quad, sizeof quad);
        drawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        flatProg_.setUniformValue("color", line);
        dynVbo_.allocate(border, sizeof border);
        drawArrays(GL_LINES, 0, 8);
    }
    dynVao_.release();
}

void GlRenderer::drawBodies(const RenderScene& s, bool idPass) {
    const QMatrix4x4 mvp = s.camera.viewProjection();
    const StyleLayout L = styleLayout(s.style);
    // The style (§3.4а) applies to bodies of the shown state (pickable); preview and threads — as always.
    // In wireframe, edges of pickable bodies are drawn after all faces without depth test.
    const bool deferEdges = L.edgesOverFacesInId;
    // Opaque first, then semi-transparent. In the ID buffer transparent ones only where there are no opaque ones
    // (stencil: assembly.md §5, S2-VIEW-004).
    if (idPass) glEnable(GL_STENCIL_TEST);
    for (int pass = 0; pass < 2; ++pass) {
        if (idPass && pass == 0) {
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        } else if (idPass) {
            glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        }
        for (size_t bi = 0; bi < s.bodies.size() && bi < gpu_.size(); ++bi) {
            const DrawBody& b = s.bodies[bi];
            const bool transparent = b.alpha < 1.0f;
            if ((pass == 1) != transparent) continue;
            if (idPass && !b.pickable && !b.pickAs) continue;
            GpuBody& g = *gpu_[bi];
            const bool styled = b.pickable;
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
            if (transparent && !idPass) glDepthMask(GL_FALSE);
            g.vao.bind();
            if (idPass) {
                // All body faces in one call: face pick number = base + face index (FX3-PERF-003).
                std::vector<PickTarget> targets;
                targets.reserve(b.mesh.faceRanges.size());
                for (const auto& fr : b.mesh.faceRanges) targets.push_back(b.pickAs ? *b.pickAs : PickTarget{PickTarget::Kind::Face, b.mesh.bodyId, fr.name});
                idProg_.bind();
                idProg_.setUniformValue("mvp", mvp);
                idProg_.setUniformValue("base", float(pickBlock(targets)));
                drawElements(GL_TRIANGLES, 0, g.indexCount);
            } else {
                meshProg_.bind();
                meshProg_.setUniformValue("mvp", mvp);
                meshProg_.setUniformValue("viewDir", s.camera.direction);
                meshProg_.setUniformValue("ambient", styled && !L.shadedFaces ? 1.0f : float(AMBIENT));
                // Face style: color and the "no depth write" flag (wireframe highlight). Consecutive faces of the same
                // style — in one call (FX3-PERF-003), the drawing order is unchanged.
                struct FaceStyle {
                    QVector4D color;
                    bool noDepth = false;
                    bool operator==(const FaceStyle& o) const { return color == o.color && noDepth == o.noDepth; }
                };
                std::vector<FaceStyle> styles;
                auto styleOf = [&](size_t fi) -> int {
                    const auto& fr = b.mesh.faceRanges[fi];
                    const PickTarget t{PickTarget::Kind::Face, b.mesh.bodyId, fr.name};
                    const bool sel = b.pickAs ? s.selection.count(*b.pickAs) > 0 : b.pickable && s.selection.count(t);
                    const bool hov = b.pickAs ? s.hovered(*b.pickAs) : b.pickable && s.hovered(t);
                    const bool emph = b.pickAs ? s.emphasized(*b.pickAs) : b.pickable && s.emphasized(t);
                    QColor base = b.color;
                    if (styled && !L.shadedFaces) base = COLOR_HLR_FACE.color;
                    else if (const auto fc = b.faceColors.find(fr.name); fc != b.faceColors.end()) base = fc->second;
                    FaceStyle st{rgba(base, b.alpha)};
                    if (styled && !L.drawFaces) {  // wireframe: a face is visible only via highlight
                        if (!sel && !hov) return -1;
                        st.color = emph  ? rgba(COLOR_LIST_HOVER.color, float(LIST_HOVER_ALPHA))
                                   : sel ? rgba(COLOR_SELECTED.color, float(FACE_SELECTED_ALPHA))
                                         : rgba(COLOR_HOVER.color, float(FACE_HOVER_ALPHA));
                        st.noDepth = true;
                    } else if (emph) st.color = rgba(mix(base, COLOR_LIST_HOVER.color, LIST_HOVER_ALPHA), b.alpha);
                    else if (sel) st.color = rgba(mix(base, COLOR_SELECTED.color, FACE_SELECTED_ALPHA), b.alpha);
                    else if (hov) st.color = rgba(mix(base, COLOR_HOVER.color, FACE_HOVER_ALPHA), b.alpha);
                    const auto it = std::find(styles.begin(), styles.end(), st);
                    if (it != styles.end()) return int(it - styles.begin());
                    styles.push_back(st);
                    return int(styles.size() - 1);
                };
                const size_t nf = std::min(g.faceRanges.size(), b.mesh.faceRanges.size());
                const std::vector<std::pair<int, int>> ranges(g.faceRanges.begin(), g.faceRanges.begin() + long(nf));
                for (const DrawRun& run : drawRuns(ranges, styleOf)) {
                    const FaceStyle& st = styles[size_t(run.style)];
                    if (st.noDepth) glDepthMask(GL_FALSE);  // as before: from the first highlighted wireframe face to the end
                    meshProg_.setUniformValue("color", st.color);
                    drawElements(GL_TRIANGLES, run.first, run.count);
                }
            }
            g.vao.release();
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDepthMask(GL_TRUE);
            if (!(deferEdges && styled)) drawBodyEdges(s, b, g, idPass);
        }
    }
    if (idPass) glDisable(GL_STENCIL_TEST);
    if (deferEdges) {
        glDisable(GL_DEPTH_TEST);
        for (size_t bi = 0; bi < s.bodies.size() && bi < gpu_.size(); ++bi)
            if (s.bodies[bi].pickable) drawBodyEdges(s, s.bodies[bi], *gpu_[bi], idPass);
        glEnable(GL_DEPTH_TEST);
    }
}

void GlRenderer::drawBodyEdges(const RenderScene& s, const DrawBody& b, GpuBody& g, bool idPass) {
    const StyleLayout L = styleLayout(s.style);
    const bool transparent = b.alpha < 1.0f;
    g.edgeVao.bind();
    if (idPass) {
        // All body edges in one call (FX3-PERF-003); the section cut outline (unnamed) is not pickable (§3.4).
        std::vector<PickTarget> targets;
        targets.reserve(b.mesh.edges.size());
        for (const auto& e : b.mesh.edges) targets.push_back({PickTarget::Kind::Edge, b.mesh.bodyId, e.name});
        // EDGE_ID_WIDTH_PX quads, as wide edges of the color pass: the same width on every context (glLineWidth > 1 works
        // on compatibility contexts only — the pick buffer differed between drivers).
        const uint32_t base = pickBlock(targets);
        if (wideOk_) {
            g.edgeVao.release();
            wideIdProg_.bind();
            setWideUniforms(wideIdProg_, s);
            wideIdProg_.setUniformValue("width", float(EDGE_ID_WIDTH_PX) * s.pixelRatio);
            wideIdProg_.setUniformValue("base", float(base));
            g.wideVao.bind();
            drawArrays(GL_TRIANGLES, 0, g.edgeVertexCount * 3);
            g.wideVao.release();
            return;
        }
        idProg_.bind();
        idProg_.setUniformValue("mvp", s.camera.viewProjection());
        idProg_.setUniformValue("base", float(base));
        drawArrays(GL_LINES, 0, g.edgeVertexCount);
        g.edgeVao.release();
        return;
    }
    flatProg_.bind();
    flatProg_.setUniformValue("mvp", s.camera.viewProjection());
    // Edge style: 0 — normal, 1 — selected, 2 — hovered, 3 — a hovered selection list row (FX10-WZ-002); consecutive
    // edges of the same style — in one call (FX3-PERF-003), the drawing order is unchanged.
    auto styleOf = [&](size_t ei) -> int {
        const PickTarget t{PickTarget::Kind::Edge, b.mesh.bodyId, b.mesh.edges[ei].name};
        if (b.pickable && s.emphasized(t)) return 3;
        if (b.pickable && s.selection.count(t)) return 1;
        if (b.pickable && s.hovered(t)) return 2;
        if (b.pickable && !L.visibleEdges) return -1;  // «shaded»: edges are not drawn
        return 0;
    };
    const QVector4D plain = b.edgeColor ? rgba(*b.edgeColor) : transparent ? rgba(b.color) : rgba(COLOR_EDGE.color);
    const QVector4D colors[4] = {plain, rgba(COLOR_SELECTED.color), rgba(COLOR_HOVER.color), rgba(COLOR_LIST_HOVER.color)};
    const float widths[4] = {float(EDGE_WIDTH_PX), float(EDGE_SELECTED_WIDTH_PX), float(EDGE_HOVER_WIDTH_PX), float(EDGE_SELECTED_WIDTH_PX)};
    // Wide edges (selected, hovered; §3.5, FX7-PT-001): the core profile does not guarantee glLineWidth > 1 (many
    // drivers clamp it to 1, so the selection looked thin) — screen-space quads of the vertex shader, one call per run.
    for (const DrawRun& run : drawRuns(g.edgeRanges, styleOf)) {
        const float w = widths[run.style] * s.pixelRatio;  // device pixels
        if (run.style == 0 || widths[run.style] <= 1.5f || !wideOk_) {
            flatProg_.bind();
            flatProg_.setUniformValue("color", colors[run.style]);
            glLineWidth(w);
            g.edgeVao.bind();
            drawArrays(GL_LINES, run.first, run.count);
            continue;
        }
        wideProg_.bind();
        setWideUniforms(wideProg_, s);
        wideProg_.setUniformValue("width", w);
        wideProg_.setUniformValue("depthBias", WIDE_EDGE_DEPTH_BIAS);
        wideProg_.setUniformValue("color", colors[run.style]);
        g.wideVao.bind();
        drawArrays(GL_TRIANGLES, run.first * 3, run.count * 3);  // 6 vertices of a segment for its 2 line vertices
    }
    glLineWidth(1.0f);
    g.wideVao.release();
    g.edgeVao.release();
}

void GlRenderer::setWideUniforms(QOpenGLShaderProgram& prog, const RenderScene& s) {
    GLint vp[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, vp);
    prog.setUniformValue("mvp", s.camera.viewProjection());
    prog.setUniformValue("viewport", QVector2D(float(std::max(1, vp[2])), float(std::max(1, vp[3]))));
}

void GlRenderer::drawHiddenEdges(const RenderScene& s, const QSize& sz) {
    // Edges behind faces (face depth is already in the buffer): GL_GREATER test, no depth write.
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_FALSE);
    dashProg_.bind();
    dashProg_.setUniformValue("mvp", s.camera.viewProjection());
    dashProg_.setUniformValue("viewport", QVector2D(float(sz.width()), float(sz.height())));
    dashProg_.setUniformValue("dash", QVector2D(HIDDEN_DASH_ON_PX, HIDDEN_DASH_OFF_PX) * s.pixelRatio);
    dashProg_.setUniformValue("color", rgba(COLOR_EDGE_HIDDEN.color));
    glLineWidth(1.0f);
    for (size_t bi = 0; bi < s.bodies.size() && bi < gpu_.size(); ++bi) {
        if (!s.bodies[bi].pickable) continue;
        GpuBody& g = *gpu_[bi];
        g.edgeVao.bind();
        drawArrays(GL_LINES, 0, g.edgeVertexCount);  // all body edges in one call (FX3-PERF-003)
        g.edgeVao.release();
    }
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

IdFrame GlRenderer::renderIdPass(const RenderScene& s, const QSize& sz) {
    if (!idFbo_ || idFbo_->size() != sz) {
        QOpenGLFramebufferObjectFormat f;
        f.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        idFbo_ = std::make_unique<QOpenGLFramebufferObject>(sz, f);
    }
    idFbo_->bind();
    glViewport(0, 0, sz.width(), sz.height());
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    pickTable_.clear();
    // Planes first and without depth write (if there are bodies): bodies cover them in the ID buffer even behind the
    // plane — a plane is pickable only where there are no bodies (app-ux §3.5).
    drawPlanes(s, true);
    drawBodies(s, true);
    drawRegions(s, true);
    IdFrame out;
    out.depth.assign(size_t(sz.width()) * sz.height(), 1.0f);
    glReadPixels(0, 0, sz.width(), sz.height(), GL_DEPTH_COMPONENT, GL_FLOAT, out.depth.data());
    out.image = idFbo_->toImage().convertToFormat(QImage::Format_RGBA8888);
    idFbo_->release();
    out.table = pickTable_;
    return out;
}

void GlRenderer::renderColor(const RenderScene& s, const QSize& sz, GLuint fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, sz.width(), sz.height());
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawBackground();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    // Frame alpha stays 1: otherwise Qt composites the view over the window background and colors "fade".
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    const auto keep = pickTable_;  // the color pass does not change the pick table
    draws_ = 0;
    drawBodies(s, false);
    if (styleLayout(s.style).hiddenDashed) drawHiddenEdges(s, sz);
    drawPlanes(s, false);
    drawRegions(s, false);
    lastColorDraws_ = draws_;
    pickTable_ = keep;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

}  // namespace construe::app
