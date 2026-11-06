// Part selection and depth manipulator (app/part_tool.h).
#include "app/part_tool.h"

#include "app/main_window.h"
#include "app/sketch_geometry.h"
#include "app/text_box.h"
#include "app/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::app {

bool PartTool::overManipulator(const QPointF& p) const {
    const auto m = w_.manipulatorScreen();
    if (!m) return false;
    const QPointF a = m->first, b = m->second, ab = b - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t = l2 > 0 ? std::clamp(((p - a).x() * ab.x() + (p - a).y() * ab.y()) / l2, 0.0, 1.0) : 0.0;
    const QPointF c = a + ab * t;
    return std::hypot(p.x() - c.x(), p.y() - c.y()) <= PICK_CURVE_PX + 2;
}

bool PartTool::overFreeManip(const QPointF& p) const {
    const auto c = w_.freeManipulatorScreen();
    return c && std::hypot(p.x() - c->x(), p.y() - c->y()) <= FREE_MANIP_PX * 1.25;
}

bool PartTool::mousePress(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return false;
    press_ = e->position().toPoint();
    dragInstance_.clear();
    dragging_ = false;
    pulling_ = false;
    boxArmed_ = boxing_ = false;
    if (overFreeManip(e->position()) && w_.freeManipBegin(press_)) {  // free-degree manipulator (§8а, FX8-ASM-003)
        pulling_ = true;
        return true;
    }
    pmiDrag_.clear();
    pmiMoving_ = false;
    if (w_.mode() == Mode::Part && w_.annotationsShown()) {  // annotation under the cursor: drag (FX12-PMI-002)
        auto& o = w_.annotationOverlay();
        if (const auto id = o.anchorHit(e->position())) pmiDrag_ = *id, pmiByAnchor_ = true;
        else if (const auto id2 = o.hit(e->position())) pmiDrag_ = *id2, pmiByAnchor_ = false;
        if (!pmiDrag_.empty()) {
            pmiL0_ = o.labelScreen(pmiDrag_).value_or(e->position());
            pmiA0_ = o.anchorScreen(pmiDrag_).value_or(e->position());
        }
    }
    if (w_.mode() == Mode::Part && w_.assemblyActive()) {  // assembly instance under the cursor (§8а)
        const PickTarget t = w_.viewport()->pickAt(press_, true, true, false);
        if ((t.kind == PickTarget::Kind::Face || t.kind == PickTarget::Kind::Edge) && !t.body.empty()) {
            dragInstance_ = t.body.substr(0, t.body.find('/'));
            // A derived connection fastener on the exploded view is dragged by itself (assembly.md §7д.5, FX10-ASM-004).
            if (const auto a = w_.model().session().assembly(); a && a->exploded)
                if (const auto* e = a->find(t.body.substr(0, t.body.rfind('/'))); e && !e->connection.empty()) dragInstance_ = e->path;
        }
    }
    // An empty place of the assembly view: the selection box (FX13-ASM-002, §8а).
    if (w_.mode() == Mode::Part && w_.assemblyActive() && dragInstance_.empty() && pmiDrag_.empty() && !overManipulator(e->position()))
        boxArmed_ = true;
    if (overManipulator(e->position()) && w_.mode() == Mode::Section) {  // section offset (§5.11)
        if (auto d = w_.sectionPanel()->offsetMm()) {
            const auto m = *w_.manipulatorScreen();
            manip_ = true;
            d0_ = *d;
            rev0_ = w_.sectionPanel()->definition().value("flip", false);
            const QPointF ab = m.second - m.first;
            const double len = std::hypot(ab.x(), ab.y());
            axis_ = ab / len;
            mmPerAxisPx_ = MANIP_ARROW_PX * w_.viewport()->camera().mmPerPx() / len;
        }
        return true;
    }
    if (overManipulator(e->position())) {
        auto* x = w_.extrudePanel();
        if (auto d = x->depthMm()) {
            const auto m = *w_.manipulatorScreen();
            manip_ = true;
            d0_ = *d;
            rev0_ = x->reversed();
            const QPointF ab = m.second - m.first;
            const double len = std::hypot(ab.x(), ab.y());
            axis_ = ab / len;
            mmPerAxisPx_ = MANIP_ARROW_PX * w_.viewport()->camera().mmPerPx() / len;
        }
    }
    return true;
}

std::optional<QRectF> PartTool::selectionBox() const {
    if (!boxing_) return std::nullopt;
    return QRectF(QPointF(press_), QPointF(boxCur_)).normalized();
}

// Double-click on an annotation label: edit it (§3.4, §5.12).
bool PartTool::mouseDoubleClick(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || w_.mode() != Mode::Part || !w_.annotationsShown()) return false;
    if (const auto id = w_.annotationOverlay().hit(e->position())) {
        w_.editAnnotation(*id);
        return true;
    }
    return false;
}

bool PartTool::mouseMove(QMouseEvent* e) {
    Viewport* v = w_.viewport();
    if (pulling_) {
        w_.freeManipUpdate(e->position().toPoint());
        return true;
    }
    if (const bool hf = overFreeManip(e->position()); hf != hoverFree_) {
        hoverFree_ = hf;
        v->update();
    }
    if (boxArmed_ && (e->buttons() & Qt::LeftButton)) {  // selection box (FX13-ASM-002)
        boxCur_ = e->position().toPoint();
        if (!boxing_ && (boxCur_ - press_).manhattanLength() > DRAG_THRESHOLD_PX) boxing_ = true;
        if (boxing_) {
            v->update();
            return true;
        }
    }
    if (!pmiDrag_.empty() && (e->buttons() & Qt::LeftButton)) {  // annotation drag preview (FX12-PMI-002)
        const QPoint d = e->position().toPoint() - press_;
        if (!pmiMoving_ && d.manhattanLength() > DRAG_THRESHOLD_PX) pmiMoving_ = true;
        if (pmiMoving_) {
            auto& o = w_.annotationOverlay();
            const auto L = o.labelScreen(pmiDrag_), A = o.anchorScreen(pmiDrag_);
            if (L && A) {  // the grabbed point moves by the drag (the grab offset is kept)
                const QPointF dd = e->position() - QPointF(press_);
                if (pmiByAnchor_) o.setDragPreview(pmiDrag_, pmiL0_ + dd, pmiA0_ + dd);
                else o.setDragPreview(pmiDrag_, pmiL0_ + dd, std::nullopt);
            }
            v->update();
            return true;
        }
    }
    if (!dragInstance_.empty() && (e->buttons() & Qt::LeftButton)) {
        const QPoint d = e->position().toPoint() - press_;
        if (!dragging_ && d.manhattanLength() > DRAG_THRESHOLD_PX) {
            dragging_ = w_.instanceDragBegin(dragInstance_, press_);
            if (!dragging_) dragInstance_.clear();  // fixed: one notification per press
        }
        if (dragging_) {
            w_.instanceDragUpdate(d, e->modifiers() & Qt::ShiftModifier);
            return true;
        }
    }
    if (manip_) {
        const QPointF d = e->position() - QPointF(press_);
        const double along = d.x() * axis_.x() + d.y() * axis_.y();
        double depth = d0_ + along * mmPerAxisPx_ * (w_.mode() != Mode::Section && w_.extrudePanel()->symmetric() ? 2 : 1);
        if (!(e->modifiers() & Qt::ShiftModifier)) {
            // Step of the "nice" series {1, 2, 5}·10^k mm, no less than MANIP_SNAP_PX pixels (§5.8).
            const double minStep = MANIP_SNAP_PX * v->camera().mmPerPx();
            double step = std::pow(10.0, std::floor(std::log10(minStep)));
            for (double k : {1.0, 2.0, 5.0, 10.0})
                if (k * step >= minStep) {
                    step *= k;
                    break;
                }
            depth = std::round(depth / step) * step;
        }
        if (w_.mode() == Mode::Section) {
            // Arrow points to the side cut off; with «Flip» it is against the plane normal
            // while the offset is along the normal.
            const double along2 = depth - d0_;
            w_.sectionPanel()->setOffsetFromManipulator(d0_ + (rev0_ ? -along2 : along2));
            hoverManip_ = true;
            v->update();
            return true;
        }
        auto* x = w_.extrudePanel();
        bool wantReverse = rev0_;
        if (!x->symmetric() && depth < 0) wantReverse = !rev0_;
        const double mag = std::max(std::abs(depth), kernel::KERNEL_MIN_DIMENSION);
        x->setDepthFromManipulator(mag, wantReverse != x->reversed());
        hoverManip_ = true;
        v->update();
        return true;
    }
    const bool hm = overManipulator(e->position());
    if (hm != hoverManip_) {
        hoverManip_ = hm;
        v->update();
    }
    // Picking a sketch base (app-ux §6.1, FX1-SKI-003): planes and faces; edges, vertices and sketch elements build a
    // plane by selection (FX7-SEL-001).
    v->setHover(v->pickAt(e->position().toPoint(), true, true, true));
    return true;
}

bool PartTool::mouseRelease(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return false;
    if (pulling_) {
        pulling_ = false;
        // A click without a drag on the manipulator is an ordinary click (a pick of the face under it, FX9-ASM-003).
        if ((e->position().toPoint() - press_).manhattanLength() > DRAG_THRESHOLD_PX) {
            w_.freeManipEnd();
            return true;
        }
        w_.freeManipCancel();
    }
    if (boxing_) {  // selection box (FX13-ASM-002): left to right — entirely inside, right to left — touched
        boxing_ = boxArmed_ = false;
        boxCur_ = e->position().toPoint();
        w_.boxSelectInstances(QRectF(QPointF(press_), QPointF(boxCur_)).normalized(), boxCur_.x() >= press_.x(),
                              e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier), boxCur_);
        w_.viewport()->update();
        return true;
    }
    boxArmed_ = false;
    if (pmiMoving_) {  // annotation drag: commit (FX12-PMI-002)
        const std::string id = pmiDrag_;
        pmiDrag_.clear();
        pmiMoving_ = false;
        const QPointF dd = e->position() - QPointF(press_);
        w_.annotationDragEnd(id, pmiByAnchor_, ((pmiByAnchor_ ? pmiA0_ : pmiL0_) + dd).toPoint());
        return true;
    }
    pmiDrag_.clear();
    if (dragging_) {
        dragging_ = false;
        dragInstance_.clear();
        w_.instanceDragEnd();
        return true;
    }
    dragInstance_.clear();
    if (manip_) {
        manip_ = false;
        return true;
    }
    if ((e->position().toPoint() - press_).manhattanLength() > DRAG_THRESHOLD_PX) return true;
    Viewport* v = w_.viewport();
    const bool pick = w_.mode() == Mode::PickPlane;
    w_.noteViewClick(e->position().toPoint());  // the hint at the selection set appears next to the click (selection.md §5.3)
    const PickTarget t = v->pickAt(e->position().toPoint(), true, true, true);
    if (w_.mode() == Mode::FeatureEdit) {
        w_.pickedInFeatureEdit(t);
        return true;
    }
    if (w_.mode() == Mode::Mate) {  // mate object (§8а)
        w_.pickedInMate(t);
        return true;
    }
    if (w_.mode() == Mode::Solution || w_.mode() == Mode::SolutionRecord) {  // a connection input (§8д, §8е), the click point for points
        w_.pickedInSolution(t, v->pointUnder(e->position().toPoint()));
        return true;
    }
    if (w_.mode() == Mode::Library) {  // journal / shoulder of the seating on a shaft (§8г, S6A-BRG-003)
        w_.pickedInLibrary(t);
        return true;
    }
    if (w_.mode() == Mode::Repair) {  // a replacement of a lost reference (§5.5в, S7F-FEAT-003)
        w_.pickedInRepair(t);
        return true;
    }
    if (w_.mode() == Mode::Weld) {  // weld edge (§8а, S5-WELD-001)
        w_.pickedInWeld(t);
        return true;
    }
    if (w_.mode() == Mode::InstancePattern) {  // source, direction, axis or part instance of a pattern (§8а, FX7-ASM-007)
        w_.pickedInInstancePattern(t);
        return true;
    }
    if (w_.mode() == Mode::Section) {  // plane or planar face of the section (§5.11)
        w_.pickedInSection(t);
        return true;
    }
    if (w_.mode() == Mode::Annotation) {  // annotation face (§5.12)
        w_.pickedInAnnotation(t, v->pointUnder(e->position().toPoint()));  // click point is the anchor (FX1-FEAT-005)
        return true;
    }
    if (w_.mode() == Mode::Properties) {  // face for «Face color» (§5.10)
        w_.faceClickedInProperties(t);
        return true;
    }
    if (pick) {  // a plane or a planar face — the sketch at once; otherwise (or Ctrl) — the selection set (§6.1, FX7-SEL-001)
        w_.pickedInPickPlane(t, e->modifiers() & Qt::ControlModifier);
        return true;
    }
    std::set<PickTarget> sel = v->selection();
    const bool add = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    w_.noteInstanceClick(sel, t, add);  // the manipulator of a nested part (FX9-ASM-010)
    if (t.kind == PickTarget::Kind::None) {
        if (!add) sel.clear();
    } else if (add) {
        if (!sel.erase(t)) sel.insert(t);
    } else {
        sel = {t};
    }
    v->setSelection(sel);
    // Plane, origin, axis in the view ↔ tree item (§4.4); the tree signal must not reset the selection in the view.
    const QSignalBlocker block(w_.tree());
    w_.tree()->clearSelection();
    for (const auto& s : sel)
        if (s.kind == PickTarget::Kind::Plane || s.kind == PickTarget::Kind::Origin || s.kind == PickTarget::Kind::Axis ||
            s.kind == PickTarget::Kind::Weld)  // weld ↔ its tree node (§8а)
            if (QTreeWidgetItem* it = w_.tree()->itemById(s.name)) it->setSelected(true);
    w_.tree()->viewport()->update();
    w_.viewSelectionPicked(e->position().toPoint());
    return true;
}

void PartTool::paint(QPainter& p) {
    if (const auto m = w_.manipulatorScreen()) {
        const QColor c = (hoverManip_ || manip_) ? COLOR_HOVER.color : COLOR_MANIP.color;
        p.setPen(QPen(c, 2.5));
        p.drawLine(m->first, m->second);
        const QPointF ab = m->second - m->first;
        const double len = std::hypot(ab.x(), ab.y());
        const QPointF u = ab / len, nrm(-u.y(), u.x());
        QPolygonF cone{m->second + u * 12, m->second + nrm * 5, m->second - nrm * 5};
        p.setBrush(c);
        p.drawPolygon(cone);
        p.setBrush(Qt::NoBrush);
    }
    paintFreeManip(p);
    if (boxing_) {  // selection box (FX13-ASM-002, §8а): a highlighted rectangle
        QColor fill = COLOR_SELECTED.color;
        fill.setAlpha(BOX_FILL_ALPHA);
        p.setPen(QPen(COLOR_SELECTED.color, 1, boxCur_.x() >= press_.x() ? Qt::SolidLine : Qt::DashLine));
        p.setBrush(fill);
        p.drawRect(QRectF(QPointF(press_), QPointF(boxCur_)).normalized());
        p.setBrush(Qt::NoBrush);
    }
    // Technical-requirement annotations (§3.4, S3-PMI-*): on top of bodies, in the screen plane.
    // Depth test (FX12-PMI-001): annotations of faces hidden by the model are not drawn.
    if (w_.annotationsShown())
        w_.annotationOverlay().paint(p, w_.viewport()->camera(), w_.selectedAnnotation(),
                                     [this](const QVector3D& q) { return w_.viewport()->pointVisible(q); });
    if (w_.mode() == Mode::Sketch) return;
    paintSelection(p);  // pickable sketch elements and the candidate preview (FX7-SEL-001)
    // Sketches outside sketch mode: visible ones (core-api visibleSketches, app-ux §3.4), no construction geometry.
    auto& session = w_.model().session();
    const auto d = session.document().document;
    const Camera& cam = w_.viewport()->camera();
    for (const auto& id : w_.tree()->shownSketches()) {  // honoring the tree eye (§4.2а, FX1-UI-009)
        const auto f = d->active().find(id);
        auto st = session.sketchState(id);
        if (!f || !st) continue;
        auto S = [&](const Point2& q) {
            const Point3 wp = st->frame.toWorld(q);
            return cam.project(QVector3D(float(wp.x()), float(wp.y()), float(wp.z())));
        };
        auto P = [&](const std::string& pid) {
            auto it = st->result.state.points.find(pid);
            return it != st->result.state.points.end() ? it->second : Point2::Zero();
        };
        p.setPen(QPen(COLOR_SK_FULL.color, SK_LINE_PX));
        for (const auto& e : f->sketchData()->entities) {
            if (e.construction) continue;
            if (e.type == sketch::EntityType::Line) p.drawLine(S(P(e.p1)), S(P(e.p2)));
            if (e.type == sketch::EntityType::Text)  // text: as contours (§6.11в)
                for (const auto& pl : textPolylines(e, P(e.center))) {
                    QPainterPath path;
                    for (size_t i = 0; i < pl.size(); ++i) i == 0 ? path.moveTo(S(pl[i])) : path.lineTo(S(pl[i]));
                    p.drawPath(path);
                }
            if (sketch::isEllipseType(e.type)) {  // S4-SKT-004, -007
                auto it = st->result.state.ellipses.find(e.id);
                const auto sh = it != st->result.state.ellipses.end() ? it->second : sketch::SolvedState::EllipseShape{e.rx, e.ry, e.angle};
                QPainterPath path;
                const auto pl = e.type == sketch::EntityType::EllipseArc ? ellipseArcPolyline(P(e.center), sh, P(e.start), P(e.end), 96)
                                                                         : ellipsePolyline(P(e.center), sh, 96);
                for (size_t i = 0; i < pl.size(); ++i) i == 0 ? path.moveTo(S(pl[i])) : path.lineTo(S(pl[i]));
                p.drawPath(path);
            }
            if (e.type == sketch::EntityType::Circle || e.type == sketch::EntityType::Arc) {
                const Point2 c = P(e.center);
                double r = e.r, a0 = 0, a1 = 2 * std::numbers::pi;
                if (auto it = st->result.state.radii.find(e.id); it != st->result.state.radii.end()) r = it->second;
                if (e.type == sketch::EntityType::Arc) {
                    const Point2 s0 = P(e.start) - c, s1 = P(e.end) - c;
                    r = s0.norm();
                    a0 = std::atan2(s0.y(), s0.x());
                    a1 = std::atan2(s1.y(), s1.x());
                    if (a1 <= a0) a1 += 2 * std::numbers::pi;
                }
                QPainterPath path;
                for (int i = 0; i <= 96; ++i) {
                    const double a = a0 + (a1 - a0) * i / 96;
                    const QPointF q = S(c + Point2(r * std::cos(a), r * std::sin(a)));
                    if (i == 0) path.moveTo(q);
                    else path.lineTo(q);
                }
                p.drawPath(path);
            }
        }
    }
}

// Free-degree manipulator icon (§8а, FX8-ASM-003): four arrows in a cross and a circular arrow around them.
void PartTool::paintFreeManip(QPainter& p) {
    const auto c = w_.freeManipulatorScreen();
    if (!c) return;
    const QColor col = (hoverFree_ || pulling_) ? COLOR_HOVER.color : COLOR_MANIP.color;
    const double a = FREE_MANIP_PX * 0.6, h = 5;
    p.setPen(QPen(col, 2));
    p.setBrush(col);
    for (const QPointF u : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) {
        const QPointF tip = *c + u * a, nrm(-u.y(), u.x());
        p.drawLine(*c, tip);
        p.drawPolygon(QPolygonF{tip + u * h * 1.6, tip + nrm * h, tip - nrm * h});
    }
    p.setBrush(Qt::NoBrush);
    const double r = FREE_MANIP_PX;
    const QRectF box(c->x() - r, c->y() - r, 2 * r, 2 * r);
    p.drawArc(box, 30 * 16, 300 * 16);
    const double end = 330 * std::numbers::pi / 180;  // the arrowhead at the end of the arc (counterclockwise on screen)
    const QPointF tip(c->x() + r * std::cos(end), c->y() - r * std::sin(end));
    const QPointF tan(std::sin(end), std::cos(end));
    const QPointF nrm(-tan.y(), tan.x());
    p.setBrush(col);
    p.drawPolygon(QPolygonF{tip + tan * h * 1.6, tip + nrm * h, tip - nrm * h});
    p.setBrush(Qt::NoBrush);
}

// Pickable elements of visible sketches (construction lines dashed, points as squares; hover and selection), and the
// candidate preview «as a grey dimension» (selection.md §2.4): a plane — a square with the normal arrow, an axis —
// dash-dot, a direction — a dashed arrow.
void PartTool::paintSelection(QPainter& p) {
    Viewport* v = w_.viewport();
    const Camera& cam = v->camera();
    auto hl = [&](const DrawSketchPick& sp) -> std::optional<QColor> {
        const PickTarget t{PickTarget::Kind::SketchEntity, sp.sketch, sp.entity};
        if (v->selection().count(t)) return COLOR_SELECTED.color;
        if (v->hovered(t)) return COLOR_HOVER.color;
        return std::nullopt;
    };
    for (const auto& sp : v->sketchPicks()) {
        const auto c = hl(sp);
        if (sp.kind == DrawSketchPick::Kind::Point) {
            const QPointF q = cam.project(sp.points[0]);
            const double r = c ? SK_POINT_PX : SK_POINT_PX / 2.0;
            p.fillRect(QRectF(q.x() - r / 2, q.y() - r / 2, r, r), c ? *c : COLOR_SK_FULL.color);
            continue;
        }
        if (!sp.construction && !c) continue;  // regular curves are drawn with the sketch below
        QPen pen(c ? *c : COLOR_DIM_WEAK.color, c ? EDGE_SELECTED_WIDTH_PX : 1.0, sp.construction ? Qt::DashDotLine : Qt::SolidLine);
        p.setPen(pen);
        QPainterPath path;
        for (size_t i = 0; i < sp.points.size(); ++i) i == 0 ? path.moveTo(cam.project(sp.points[i])) : path.lineTo(cam.project(sp.points[i]));
        p.drawPath(path);
    }
    bool pending = false;
    const api::SelectionPreview* pr = w_.selectionPreview(&pending);
    if (!pr) return;
    QColor col = COLOR_DIM_WEAK.color;
    auto P = [&](const Point3& q) { return cam.project(QVector3D(float(q.x()), float(q.y()), float(q.z()))); };
    auto arrow = [&](const QPointF& a, const QPointF& b, Qt::PenStyle style) {
        p.setPen(QPen(col, 2, style));
        p.drawLine(a, b);
        const QPointF ab = b - a;
        const double len = std::hypot(ab.x(), ab.y());
        if (len < 1) return;
        const QPointF u = ab / len, n(-u.y(), u.x());
        p.setBrush(col);
        p.setPen(QPen(col, 1));
        p.drawPolygon(QPolygonF{b + u * 10, b + n * 4, b - n * 4});
        p.setBrush(Qt::NoBrush);
    };
    const double half = v->planeHalf();
    if (pr->plane) {
        const Plane& pl = *pr->plane;
        // The square around the projection of the view center onto the plane.
        const Point3 c0(cam.center.x(), cam.center.y(), cam.center.z());
        const Point3 c = c0 - (c0 - pl.origin).dot(pl.normal) * pl.normal;
        QPolygonF sq;
        for (auto [a, b] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) sq << P(c + half * (a * pl.x + b * pl.y));
        QColor fill = col;
        fill.setAlphaF(CANDIDATE_ALPHA);
        p.setBrush(fill);
        p.setPen(QPen(col, 1.5, Qt::DashLine));
        p.drawPolygon(sq);
        p.setBrush(Qt::NoBrush);
        arrow(P(c), P(c + 0.4 * half * pl.normal), Qt::SolidLine);
    }
    if (pr->axis) {
        const auto& [pt, d] = *pr->axis;
        p.setPen(QPen(col, 2, Qt::DashDotLine));
        p.drawLine(P(pt - half * d), P(pt + half * d));
    }
    if (pr->dir) {
        const auto& [pt, d] = *pr->dir;
        arrow(P(pt), P(pt + 0.5 * half * d), Qt::DashLine);
    }
}

}  // namespace construe::app
