// 3D view scene builder (app/scene_builder.h).
#include "app/scene_builder.h"

#include "app/busy_indicator.h"
#include "app/captions.h"
#include "app/messages.h"

#include "app/theme.h"

#include <functional>
#include <numbers>
#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {

// Chord bounds for a body with diagonal diag (§11.6 item 3).
std::pair<double, double> chordBounds(double diag) {
    const double lo = diag > 0 ? std::max(kernel::KERNEL_MIN_CHORD, VIEW_MIN_CHORD_REL * diag) : kernel::KERNEL_MIN_CHORD;
    const double hi = diag > 0 ? std::min(kernel::KERNEL_MAX_CHORD, VIEW_MAX_CHORD_REL * diag) : kernel::KERNEL_MAX_CHORD;
    return {lo, std::max(lo, hi)};
}

}  // namespace

double SceneBuilder::chordFor(double mmPerPx, double diag) {
    const double t = VIEW_CHORD_PX * mmPerPx;
    // Levels VIEW_CHORD_BASE_MM · STEP^k; margin 1e-9 — a level equal to t is not lost to logarithm rounding.
    const double k = std::floor(std::log(t / VIEW_CHORD_BASE_MM) / std::log(VIEW_CHORD_STEP) + 1e-9);
    const double c = VIEW_CHORD_BASE_MM * std::pow(VIEW_CHORD_STEP, k);
    const auto [lo, hi] = chordBounds(diag);
    return std::clamp(c, lo, hi);
}

double SceneBuilder::acceptChordFor(double mmPerPx, double diag) {
    const double t = VIEW_CHORD_PX * mmPerPx;
    return std::max(chordFor(mmPerPx, diag), std::min(VIEW_CHORD_REUSE * t, chordBounds(diag).second));
}

double SceneBuilder::threadRingRadius(double r, bool internal) {
    // Mesh chords of a cylinder lie inside its circle by up to r·(1 − cos(angle/2)); ring chords — by the same rule.
    if (internal) return r * (std::cos(VIEW_ANGLE_TOL / 2) - THREAD_RING_GAP);
    return r * (1 + THREAD_RING_GAP) / std::cos(std::numbers::pi / THREAD_RING_SEGMENTS);
}

bool SceneBuilder::needsRetessellation(const std::vector<DrawBody>& bodies, double mmPerPx) {
    for (const auto& b : bodies)
        if (b.mesh.chordTol > 0 && b.mesh.chordTol > acceptChordFor(mmPerPx, b.diag) * (1 + 1e-9)) return true;
    return false;
}

std::vector<DrawBody> SceneBuilder::bodiesOf(const api::StateRef& ref, const QColor& color, float alpha, bool pickable,
                                             const std::string& keyPrefix, double mmPerPx, const std::vector<api::Json>* sections) const {
    std::vector<DrawBody> out;
    auto infos = s_.bodies(ref);
    if (!infos) return out;
    for (const auto& b : *infos) {
        busyTick();  // tessellation in the interface thread: the long work stripe moves (app-ux §9.4)
        const double diag = b.bbox.isEmpty() ? 0 : b.bbox.diagonal().norm();
        kernel::TessParams tp{chordFor(mmPerPx, diag), VIEW_ANGLE_TOL};
        const double accept = acceptChordFor(mmPerPx, diag);  // ready mesh (§11.6, FX3-PERF-002)
        auto m = sections && !sections->empty() ? s_.sectionTessellation(b.bodyId, ref, tp, *sections, accept)
                                                : s_.tessellation(b.bodyId, ref, tp, accept);
        if (!m) continue;
        DrawBody d;
        d.diag = diag;
        d.key = keyPrefix + "/" + b.bodyId + "/" + std::to_string(m->chordTol) + "/" +
                std::to_string(reinterpret_cast<uintptr_t>(m->positions.data())) + std::to_string(m->positions.size());
        d.mesh = std::move(*m);
        d.color = color;
        d.alpha = alpha;
        d.pickable = pickable;
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<api::Json> SceneBuilder::viewSections(const std::vector<api::Session::SectionInfo>& all, const std::optional<std::string>& editing,
                                                  const api::Json* panelDef) {
    std::vector<api::Json> out;
    for (const auto& s : all)
        if (!s.hidden && !(panelDef && editing && *editing == s.id)) out.push_back(s.def);
    if (panelDef) out.push_back(*panelDef);
    return out;
}

SceneContent SceneBuilder::build(const SceneRequest& r) const {
    SceneContent out;
    const auto d = s_.document().document;
    const auto& el = d->active();
    const std::string rev = std::to_string(r.revision);
    auto beforeRef = [&](const std::string& featureId) {
        const int i = el.indexOf(featureId);
        return i <= 0 ? api::StateRef::initial() : api::StateRef::after(el.features[i - 1]->id);
    };
    if (r.mode == SceneRequest::Mode::Sketch) {
        // State before the sketch; edges and vertices — for projection and snapping (§6.11а, §6.4).
        out.bodies = bodiesOf(beforeRef(r.sketch), COLOR_BODY.color, 1.0f, true, "s" + rev, r.mmPerPx, &r.sections);
    } else if (r.mode == SceneRequest::Mode::FeatureEdit) {
        const api::StateRef before = r.editing ? beforeRef(*r.editing) : api::StateRef::final();
        out.bodies = bodiesOf(before, COLOR_BODY.color, r.operation == "intersect" ? float(PREVIEW_ALPHA) : 1.0f, true, "b" + rev, r.mmPerPx, &r.sections);
        if (r.preview) {
            const std::string& pid = *r.preview;
            std::vector<DrawBody> pv;
            if (r.operation == "cut")
                pv = bodiesOf({api::StateRef::Kind::PreviewTool, pid}, COLOR_PREVIEW_CUT.color, float(PREVIEW_ALPHA), false, "pt" + pid, r.mmPerPx);
            else if (r.operation == "intersect")
                pv = bodiesOf({api::StateRef::Kind::Preview, pid}, COLOR_PREVIEW_INTERSECT.color, float(PREVIEW_ALPHA), false, "p" + pid, r.mmPerPx);
            else
                pv = bodiesOf({api::StateRef::Kind::Preview, pid}, COLOR_PREVIEW_ADD.color, float(PREVIEW_ALPHA), false, "p" + pid, r.mmPerPx);
            for (auto& b : pv) out.bodies.push_back(std::move(b));
        }
    } else if (el.isAssembly()) {
        out.bodies = assemblyBodies(r);  // assembly instances (assembly.md §5)
        if (r.mode == SceneRequest::Mode::Part) addWelds(r, out);
    } else {
        // Flat pattern instead of the folded sheet part (§7.4, S5-FLAT-001); no flat pattern — the part itself.
        const bool flat = r.unfolded && s_.bodies(api::StateRef::flat()).ok();
        out.bodies = bodiesOf(flat ? api::StateRef::flat() : api::StateRef::final(), COLOR_BODY.color,
                              r.shownBases.empty() ? 1.0f : float(PREVIEW_ALPHA), true, (flat ? "flat" : "f") + rev, r.mmPerPx, &r.sections);
        // Base body for comparison (§3.4, S4-REC-009): state after the "Import" feature is opaque, the part translucent.
        for (const auto& id : r.shownBases)
            for (auto& b : bodiesOf(api::StateRef::after(id), COLOR_BASE_BODY.color, 1.0f, false, "base" + rev + "/" + id, r.mmPerPx))
                out.bodies.push_back(std::move(b));
    }
    // Part and face colors (§5.10, S2-PART-004): part color → material → COLOR_BODY; faces in faceColors — own color.
    if (!el.isAssembly()) {
        QColor partColor = COLOR_BODY.color;
        if (el.material) partColor = QColor(QString::fromStdString((*el.material)["color"].get<std::string>()));
        if (el.color) partColor = QColor(QString::fromStdString(*el.color));
        std::map<std::string, std::map<std::string, QColor>> faces;  // body → face name → color
        api::StateRef colorState = api::StateRef::final();
        if (r.mode == SceneRequest::Mode::Sketch) colorState = beforeRef(r.sketch);
        else if (r.mode == SceneRequest::Mode::FeatureEdit) colorState = r.editing ? beforeRef(*r.editing) : api::StateRef::final();
        if (auto fc = s_.faceColors(colorState))
            for (const auto& c : *fc) faces[c.bodyId][c.face] = QColor(QString::fromStdString(c.color));
        for (auto& b : out.bodies) {
            if (!b.pickable || b.color != COLOR_BODY.color) continue;  // preview uses its own colors
            b.color = partColor;
            if (auto it = faces.find(b.mesh.bodyId); it != faces.end()) b.faceColors = it->second;
            b.key += "/" + partColor.name().toStdString() + "/" + std::to_string(b.faceColors.size());
        }
    }
    // Part in assembly context (app-ux §8а, S2-ASM-011): context bodies are translucent and selectable.
    if (!el.isAssembly() && el.context)
        if (auto cm = s_.contextTessellation(kernel::TessParams{chordFor(r.mmPerPx, 0), VIEW_ANGLE_TOL}, acceptChordFor(r.mmPerPx, 0)))
            for (auto& m : *cm) {
                DrawBody db;
                db.key = "ctx" + rev + "/" + m.bodyId + "/" + std::to_string(reinterpret_cast<uintptr_t>(m.positions.data()));
                db.mesh = std::move(m);
                db.color = COLOR_CONTEXT.color;
                db.alpha = float(INSTANCE_TRANSPARENT_ALPHA);
                db.pickable = true;
                out.bodies.push_back(std::move(db));
            }
    // Planes: half-size from the bodies' bounding box (§3.4).
    double maxDim = 0;
    BBox3 all;
    if (auto infos = s_.bodies(api::StateRef::final()))
        for (const auto& b : *infos)
            if (!b.bbox.isEmpty()) {
                maxDim = std::max(maxDim, b.bbox.sizes().maxCoeff());
                all.extend(b.bbox);
            }
    for (const auto& b : out.bodies)  // assembly: bounding box of instances
        if (el.isAssembly())
            for (size_t i = 0; i + 2 < b.mesh.positions.size(); i += 3) {
                const Point3 p(b.mesh.positions[i], b.mesh.positions[i + 1], b.mesh.positions[i + 2]);
                all.extend(p);
                maxDim = std::max(maxDim, all.sizes().maxCoeff());
            }
    out.planeHalf = std::max(PLANE_MIN_HALF_MM, PLANE_BBOX_FACTOR * maxDim);
    // Reference geometry of the shown state (§3.4): in a sketch — before it; in a panel — preview or pre-feature state.
    api::StateRef shown = api::StateRef::final();
    if (r.mode == SceneRequest::Mode::Sketch) shown = beforeRef(r.sketch);
    else if (r.mode == SceneRequest::Mode::FeatureEdit)
        shown = r.preview ? api::StateRef{api::StateRef::Kind::Preview, *r.preview} : (r.editing ? beforeRef(*r.editing) : api::StateRef::final());
    if (auto g = s_.referenceGeometry(shown)) {
        const Point3 c = all.isEmpty() ? Point3::Zero() : Point3(all.center());
        auto nameOf = [&](const std::string& id) {
            const auto f = el.find(id);
            return QString::fromStdString(f ? f->name : id);
        };
        for (const auto& [id, pl] : g->planes) {
            const Point3 pc = pl.toWorld(pl.toLocal(c));
            out.refPlanes.push_back({id, nameOf(id), pl, QVector3D(float(pc.x()), float(pc.y()), float(pc.z()))});
        }
        for (const auto& [id, a] : g->axes)
            out.refAxes.push_back({id, nameOf(id), QVector3D(float(a.point.x()), float(a.point.y()), float(a.point.z())),
                                   QVector3D(float(a.dir.x()), float(a.dir.y()), float(a.dir.z()))});
    }
    // Nested subassemblies (FX9-ASM-008): their reference planes, axes and visible sketches as when opened, in the world
    // placement of the carrying entry; the plane centre — the projection of the bounding box centre, as for own planes.
    if (el.isAssembly() && r.mode != SceneRequest::Mode::Sketch)
        if (const auto a = s_.assembly()) {
            const Point3 c = all.isEmpty() ? Point3::Zero() : Point3(all.center());
            auto q = [](const Point3& p) { return QVector3D(float(p.x()), float(p.y()), float(p.z())); };
            for (const auto& d : s_.nestedReferenceGeometry()) {
                const auto* e = a->find(d.node);
                const QString label = QString::fromStdString((e ? e->name + ": " : std::string()) + d.name);
                const std::string id = d.node + "/" + d.id;
                if (d.kind == regen::AssemblyDatum::Kind::Plane) {
                    out.nested.planes.push_back({id, label, d.plane, q(d.plane.toWorld(d.plane.toLocal(c)))});
                } else if (d.kind == regen::AssemblyDatum::Kind::Axis) {
                    out.nested.axes.push_back({id, label, q(d.point), QVector3D(float(d.dir.x()), float(d.dir.y()), float(d.dir.z()))});
                } else {
                    for (const auto& pl : d.polylines) {
                        std::vector<QVector3D> line;
                        for (const auto& p : pl) line.push_back(q(p));
                        out.nested.sketchLines.push_back(std::move(line));
                    }
                }
            }
            // Planes of the parts in the mate mode (FX10-ASM-001): source planes, reference planes and sketch planes of
            // every part instance, IDs "<path>/<construction name>" as the mate references; the view draws and picks the
            // ones shown by the tree eye (pinned or hovered), like the planes of nested subassemblies.
            if (r.partPlanes) {
                const auto d = s_.document().document;
                for (const auto& e : a->instances) {
                    if (!e.state || e.error || e.suppressed || !e.pattern.empty() || !e.connection.empty()) continue;
                    regen::Motion3 m = e.world;  // a temporary placement of its top-level instance moves it too
                    const std::string top = e.path.substr(0, e.path.find('/'));
                    if (const auto it = r.dragMotions.find(top); it != r.dragMotions.end())
                        if (const auto* te = a->find(top)) m = it->second * regen::Motion3{te->world.R.transpose(), -(te->world.R.transpose() * te->world.t)} * e.world;
                    const int pe = e.file.empty() ? d->elementIndex(e.element) : -1;
                    auto nameOf = [&](const std::string& id) {
                        const auto f = pe >= 0 ? d->elements[size_t(pe)].find(id) : nullptr;
                        return QString::fromStdString(f ? f->name : id);
                    };
                    auto add = [&](const std::string& id, const QString& name, const Plane& local) {
                        Plane w;
                        w.origin = m.apply(local.origin);
                        w.x = m.R * local.x, w.y = m.R * local.y, w.normal = m.R * local.normal;
                        out.nested.planes.push_back({e.path + "/" + id, QString::fromStdString(e.name + ": ") + name, w,
                                                     q(w.toWorld(w.toLocal(c)))});
                    };
                    for (const auto& pd : DATUM_PLANES) add(pd.id, QString::fromUtf8(txt(pd.label)), *api::Session::datumPlane(pd.id));
                    for (const auto& [id, pl] : e.state->planes) add(id, nameOf(id), pl);
                    for (const auto& [id, sk] : e.state->sketches) add(id, nameOf(id) + txt("ui.common.plane2"), sk.frame);
                }
            }
        }
    // Cosmetic thread (§3.4, S2-FEAT-009): rings per turn — depth-tested lines, not selectable.
    if (auto threads = s_.threads(shown); threads && !threads->empty()) {
        DrawBody tb;
        tb.key = "thread" + rev + std::to_string(threads->size());
        tb.mesh.bodyId = "threads";
        tb.color = COLOR_THREAD.color;
        tb.pickable = false;
        tb.edgeColor = COLOR_THREAD.color;
        for (const auto& t : *threads) {
            int rings = int(std::floor(t.length / t.pitch)) + 1;
            int step = 1;
            while (rings / step > THREAD_MAX_RINGS) ++step;
            const Vec3 u = t.dir.unitOrthogonal(), v = t.dir.cross(u);
            const double rr = threadRingRadius(t.radius, t.internal);  // off the face mesh (FX10-HOLE-001)
            io::EdgePolylines e;
            for (int i = 0; i < rings; i += step) {
                const Point3 c = t.origin + (i * t.pitch) * t.dir;
                std::vector<float> pl;
                for (int k = 0; k <= THREAD_RING_SEGMENTS; ++k) {
                    const double a = 2 * std::numbers::pi * k / THREAD_RING_SEGMENTS;
                    const Point3 q = c + rr * (std::cos(a) * u + std::sin(a) * v);
                    pl.insert(pl.end(), {float(q.x()), float(q.y()), float(q.z())});
                }
                e.polylines.push_back(std::move(pl));
            }
            tb.mesh.edges.push_back(std::move(e));
        }
        out.bodies.push_back(std::move(tb));
    }
    for (const auto& b : out.bodies) out.failedFaces += b.mesh.failedFaces.size();
    return out;
}

std::vector<DrawWeld> SceneBuilder::weldLines(const doc::Element& asmEl, const std::vector<DrawBody>& bodies) {
    std::vector<DrawWeld> out;
    for (const auto& w : asmEl.welds) {
        if (!w.contains("edges")) continue;  // a weld on faces — its line comes from the core (addWelds, FX8-WLD-001)
        DrawWeld dw;
        dw.id = w.value("id", std::string());
        for (const auto& e : w["edges"]) {
            const std::string ref = e.get<std::string>();
            for (const auto& b : bodies) {
                const auto slash = b.mesh.bodyId.rfind('/');
                if (slash == std::string::npos) continue;
                const std::string path = b.mesh.bodyId.substr(0, slash + 1);
                if (ref.rfind(path, 0) != 0) continue;
                const std::string name = ref.substr(path.size());
                for (const auto& ed : b.mesh.edges) {
                    if (ed.name != name) continue;
                    for (const auto& pl : ed.polylines) {
                        std::vector<QVector3D> q;
                        for (size_t i = 0; i + 2 < pl.size(); i += 3) q.emplace_back(pl[i], pl[i + 1], pl[i + 2]);
                        if (q.size() >= 2) dw.polylines.push_back(std::move(q));
                    }
                }
            }
        }
        if (!dw.polylines.empty()) out.push_back(std::move(dw));
    }
    return out;
}

void SceneBuilder::addWelds(const SceneRequest& r, SceneContent& out) const {
    const auto a = s_.assembly();
    if (!a) return;
    auto ws = s_.weldDisplay({chordFor(r.mmPerPx, 0), VIEW_ANGLE_TOL}, r.sections);  // cut by the sections (FX9-WLD-001)
    const std::string sectionsKey = r.sections.empty() ? std::string() : "/" + std::to_string(std::hash<std::string>{}(api::Json(r.sections).dump()));
    if (!ws) return;
    for (auto& w : *ws) {
        // Dragging (§8а): the weld follows its host instance (the first reference).
        std::optional<regen::Motion3> move;
        if (auto dm = r.dragMotions.find(w.host); dm != r.dragMotions.end())
            if (const auto* t = a->find(w.host)) move = dm->second * regen::Motion3{t->world.R.transpose(), -(t->world.R.transpose() * t->world.t)};
        auto at = [&](float x, float y, float z) { return move ? move->apply(Point3(x, y, z)) : Point3(x, y, z); };
        DrawWeld dw;
        dw.id = w.id;
        for (const auto& pl : w.polylines) {
            std::vector<QVector3D> q;
            for (const auto& p : pl) {
                const Point3 m = at(float(p.x()), float(p.y()), float(p.z()));
                q.emplace_back(float(m.x()), float(m.y()), float(m.z()));
            }
            if (q.size() >= 2) dw.polylines.push_back(std::move(q));
        }
        if (!dw.polylines.empty()) out.weldLines.push_back(std::move(dw));
        if (!w.body) continue;
        DrawBody db;
        db.mesh = std::move(*w.body);
        if (move) {
            for (size_t i = 0; i + 2 < db.mesh.positions.size(); i += 3) {
                const Point3 p = at(db.mesh.positions[i], db.mesh.positions[i + 1], db.mesh.positions[i + 2]);
                for (int k = 0; k < 3; ++k) db.mesh.positions[i + size_t(k)] = float(p[k]);
            }
            for (size_t i = 0; i + 2 < db.mesh.normals.size(); i += 3) {
                const Vec3 n = move->applyDir(Vec3(db.mesh.normals[i], db.mesh.normals[i + 1], db.mesh.normals[i + 2]));
                for (int k = 0; k < 3; ++k) db.mesh.normals[i + size_t(k)] = float(n[k]);
            }
            for (auto& ed : db.mesh.edges)
                for (auto& pl : ed.polylines)
                    for (size_t i = 0; i + 2 < pl.size(); i += 3) {
                        const Point3 p = at(pl[i], pl[i + 1], pl[i + 2]);
                        for (int k = 0; k < 3; ++k) pl[i + size_t(k)] = float(p[k]);
                    }
        }
        db.mesh.edges.clear();  // the weld is shown by its body only, without edge lines (S7F-WLD-001)
        db.color = COLOR_WELD_BODY.color;
        db.pickable = false;
        db.pickAs = PickTarget{PickTarget::Kind::Weld, {}, w.id};  // a click on the body picks the weld (app-ux §8а)
        std::string key = "w" + std::to_string(r.revision) + "/" + db.mesh.bodyId + "/" + std::to_string(db.mesh.chordTol) + sectionsKey;
        if (move)
            for (int k = 0; k < 3; ++k) key += "/" + std::to_string(move->t[k]) + "/" + std::to_string(move->R(k, 0)) + std::to_string(move->R(k, 1));
        db.key = key;
        out.bodies.push_back(std::move(db));
    }
}

QColor SceneBuilder::instanceColor(const doc::Document& d, const regen::InstanceEntry& e) {
    // The instance color of the active assembly over everything (FX12-ASM-003): the top-level instance on the path.
    const std::string top = e.path.substr(0, e.path.find('/'));
    for (const auto& in : d.active().instances)
        if (in.id == top && in.color) return QColor(QString::fromStdString(*in.color));
    // A fastener placed by the connection wizard (FX13-JNT-004): reddish over the color of its library part.
    if (!e.connection.empty()) return COLOR_FASTENER.color;
    // The part element: of this document, or of the library copy (FX13-LIB-001); a linked file — not looked up here.
    const doc::Document* ed = &d;
    const bool library = doc::isLibraryRef(e.file);
    if (library) {
        const doc::LibraryPart* lp = d.libraryPart(e.file.substr(std::string(doc::LIBRARY_REF_PREFIX).size()));
        ed = lp && lp->document ? lp->document.get() : nullptr;
    } else if (!e.file.empty()) {
        ed = nullptr;
    }
    const int ei = ed ? ed->elementIndex(e.element) : -1;
    if (ei >= 0) {
        const auto& pe = ed->elements[size_t(ei)];
        if (pe.color) return QColor(QString::fromStdString(*pe.color));
        if (pe.material && pe.material->contains("color")) return QColor(QString::fromStdString((*pe.material)["color"].get<std::string>()));
    }
    return library ? COLOR_LIBRARY_PART.color : COLOR_BODY.color;
}

std::vector<DrawBody> SceneBuilder::assemblyBodies(const SceneRequest& r) const {
    std::vector<DrawBody> out;
    const auto a = s_.assembly();
    if (!a) return out;
    const auto d = s_.document().document;
    for (const auto& e : a->instances) {
        if (!e.state || e.hidden || e.suppressed || e.error) continue;
        const QColor color = instanceColor(*d, e);
        // Dragging (§8а): the instance and its descendants are at a temporary placement.
        regen::Motion3 world = e.world;
        if (!r.dragMotions.empty()) {
            // The deepest moved path containing the entry: a free unit of a subassembly moves by itself (assembly.md §2б).
            for (std::string top = e.path;;) {
                if (auto dm = r.dragMotions.find(top); dm != r.dragMotions.end())
                    if (const auto* t = a->find(top)) {
                        const regen::Motion3 inv{t->world.R.transpose(), -(t->world.R.transpose() * t->world.t)};
                        world = dm->second * (inv * e.world);
                        break;
                    }
                const size_t sl = top.rfind('/');
                if (sl == std::string::npos) break;
                top.resize(sl);
            }
        }
        for (const auto& b : e.state->bodies) {
            kernel::TessParams tp{chordFor(r.mmPerPx, 0), VIEW_ANGLE_TOL};
            const double accept = acceptChordFor(r.mmPerPx, 0);  // ready mesh (§11.6, FX3-PERF-002)
            auto m = r.sections.empty() ? s_.instanceTessellation(e.path, b.id, tp, accept)
                                        : s_.instanceSectionTessellation(e.path, b.id, tp, r.sections, accept);
            if (!m) continue;
            DrawBody db;
            db.mesh = std::move(*m);
            db.mesh.bodyId = e.path + "/" + b.id;  // instance body name (assembly.md §3)
            // World placement of the instance: vertices and normals.
            const auto& W = world;
            for (size_t i = 0; i + 2 < db.mesh.positions.size(); i += 3) {
                const Point3 p = W.apply(Point3(db.mesh.positions[i], db.mesh.positions[i + 1], db.mesh.positions[i + 2]));
                for (int k = 0; k < 3; ++k) db.mesh.positions[i + size_t(k)] = float(p[k]);
            }
            for (size_t i = 0; i + 2 < db.mesh.normals.size(); i += 3) {
                const Vec3 n = W.applyDir(Vec3(db.mesh.normals[i], db.mesh.normals[i + 1], db.mesh.normals[i + 2]));
                for (int k = 0; k < 3; ++k) db.mesh.normals[i + size_t(k)] = float(n[k]);
            }
            for (auto& ed : db.mesh.edges)
                for (auto& pl : ed.polylines)
                    for (size_t i = 0; i + 2 < pl.size(); i += 3) {
                        const Point3 p = W.apply(Point3(pl[i], pl[i + 1], pl[i + 2]));
                        for (int k = 0; k < 3; ++k) pl[i + size_t(k)] = float(p[k]);
                    }
            for (auto& v : db.mesh.vertices) {
                const Point3 p = W.apply(Point3(v.position[0], v.position[1], v.position[2]));
                for (int k = 0; k < 3; ++k) v.position[size_t(k)] = float(p[k]);
            }
            db.color = r.highlight.count(e.path) ? COLOR_INTERFERENCE.color : color;
            db.alpha = e.transparent ? float(INSTANCE_TRANSPARENT_ALPHA) : 1.0f;
            db.pickable = true;
            std::string key = "a" + std::to_string(r.revision) + "/" + db.mesh.bodyId + "/" + std::to_string(db.mesh.chordTol) + "/" + db.color.name().toStdString() +
                              std::to_string(db.alpha);
            for (int k = 0; k < 3; ++k) key += "/" + std::to_string(W.t[k]);
            for (int k = 0; k < 9; ++k) key += "/" + std::to_string(W.R(k / 3, k % 3));
            // The mesh depends on sections: face and vertex set is part of the key (else GPU buffers of another mesh).
            key += "/" + std::to_string(db.mesh.positions.size()) + "/" + std::to_string(db.mesh.faceRanges.size()) + "/" +
                   std::to_string(db.mesh.edges.size()) + "/" + std::to_string(std::hash<std::string>{}(api::Json(r.sections).dump()));
            db.key = key;
            out.push_back(std::move(db));
        }
    }
    return out;
}

std::vector<DrawRegion> SceneBuilder::regions(const SceneRequest& r) const {
    std::vector<DrawRegion> out;
    if (r.mode != SceneRequest::Mode::Part && !r.profilesPanel) return out;
    // Sketches: visible ones (core-api visibleSketches, app-ux §3.4) and the panel's sketch.
    std::set<std::string> shown;
    for (const auto& id : r.shownSketches ? *r.shownSketches : s_.visibleSketches()) shown.insert(id);
    if (r.profilesPanel && !r.panelSketch.empty()) shown.insert(r.panelSketch);
    for (const auto& id : shown) {
        auto st = s_.sketchState(id);
        if (!st) continue;
        for (const auto& reg : st->result.profiles.regions) {
            Point2 lo(1e300, 1e300), hi(-1e300, -1e300);
            auto ext = [&](const Point2& q) { lo = lo.cwiseMin(q), hi = hi.cwiseMax(q); };
            for (const auto& seg : reg.outer.segments) {
                if (seg.kind == sketch::Segment::Kind::Line) ext(seg.p1), ext(seg.p2);
                else if (seg.kind == sketch::Segment::Kind::Spline)  // poles bound the curve (S4A-SPL-003)
                    for (const auto& q : seg.curve.poles) ext(q);
                else {
                    const double m = std::max(seg.radius, seg.minorRadius);  // ellipse: by major semi-axis (S4-SKT-004)
                    ext(seg.center - Point2(m, m)), ext(seg.center + Point2(m, m));
                }
            }
            const double diag = hi.x() >= lo.x() ? (hi - lo).norm() : 0.0;
            auto m = s_.regionMesh(id, reg.regionId, chordFor(r.mmPerPx, diag));
            if (!m) continue;
            DrawRegion dr{id, reg.regionId, {}};
            for (const auto& t : m->triangles)
                for (int k : t) {
                    const Point3 w = st->frame.toWorld(m->vertices[size_t(k)]);
                    dr.triangles.push_back(QVector3D(float(w.x()), float(w.y()), float(w.z())));
                }
            out.push_back(std::move(dr));
        }
    }
    return out;
}

}  // namespace construe::app
