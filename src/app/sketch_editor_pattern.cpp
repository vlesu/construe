// Sketcher: pattern in sketch and "Unlink" (spec/app-ux.md §6.3 «Sketch pattern»; S4-SKT-001, S4-SKT-002, S4-SKT-005).
#include "app/sketch_editor.h"

#include "api/sketch_ops.h"
#include "app/app_model.h"
#include "app/messages.h"
#include "app/theme.h"
#include "base/geom2.h"

#include <cmath>
#include <map>
#include <numbers>
#include <set>

namespace construe::app {

namespace {
// Curve points (ends, center) are entity data.
std::vector<std::string> curvePoints(const sketch::Entity& e) {
    std::vector<std::string> v;
    for (const auto* p : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
        if (!p->empty()) v.push_back(*p);
    v.insert(v.end(), e.poles.begin(), e.poles.end());  // spline poles (S4A-SPL-001)
    return v;
}
}  // namespace

void SketchEditor::startPattern(bool circular) {
    const auto* d = data();
    if (!d) return;
    // Sources: selected segments, circles, arcs, standalone points; a curve point means its curve (sketch.md §2.6.3).
    std::map<std::string, std::string> owner;
    for (const auto& e : d->entities)
        for (const auto& p : curvePoints(e)) owner[p] = e.id;
    std::vector<std::string> src;
    auto add = [&](const std::string& id) {
        if (std::find(src.begin(), src.end(), id) == src.end()) src.push_back(id);
    };
    for (const auto& id : selection_) {
        const auto* e = d->findEntity(id);
        if (!e) continue;
        if (e->type == sketch::EntityType::Point) {
            const auto o = owner.find(id);
            if (o == owner.end()) add(id);
            else if (const auto* c = d->findEntity(o->second); c && c->type != sketch::EntityType::Text) add(c->id);
        } else if (e->type == sketch::EntityType::Line || e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Arc ||
                   e->type == sketch::EntityType::Ellipse) {  // ellipse — FX7-SK-012
            add(id);
        }
    }
    if (src.empty()) {
        emit hint(txt("ui.sketch.firstSelectPatternElements"));
        return;
    }
    setTool(circular ? Tool::PatternCircular : Tool::PatternLinear);
    pending_ = src;
    for (const auto& id : src) selection_.insert(id);  // the selection stays highlighted
    if (circular) {
        emit hint(txt("ui.sketch.clickTheCenterASketch"));
        view_.update();
        return;
    }
    // Default step is the sources' extent × PATTERN_STEP_FACTOR (§11.3); without an extent, PATTERN_STEP_DEFAULT_MM.
    double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300;
    auto grow = [&](double x, double y) { minx = std::min(minx, x), maxx = std::max(maxx, x), miny = std::min(miny, y), maxy = std::max(maxy, y); };
    for (const auto& id : src) {
        const auto* e = d->findEntity(id);
        if (e->type == sketch::EntityType::Point) grow(e->x, e->y);
        for (const auto& pid : curvePoints(*e))
            if (const auto* p = d->findEntity(pid)) {
                const double r = e->type == sketch::EntityType::Circle ? e->r : e->type == sketch::EntityType::Ellipse ? std::max(e->rx, e->ry) : 0;
                grow(p->x - r, p->y - r), grow(p->x + r, p->y + r);
            }
    }
    const double extent = std::max(maxx - minx, maxy - miny);
    const double mm = extent > 1e-9 ? extent * PATTERN_STEP_FACTOR : PATTERN_STEP_DEFAULT_MM;
    const auto u = model_.session().units();
    PatternBox::Values v;
    v.step1 = v.step2 = formatNumber(mm / expr::lengthFactor(u.length));
    v.angle2 = formatNumber(std::numbers::pi / 2 / expr::angleFactor(u.angle));
    v.angle1 = "0";
    openPatternBox(v, {});
}

void SketchEditor::clickPatternCenter(const QPointF& s) {
    const Hit h = hitAt(s);
    if (h.kind != Hit::Kind::Point || std::find(pending_.begin(), pending_.end(), h.id) != pending_.end()) {
        emit hint(txt("ui.sketch.requiredSketchPointOrOrigin"));
        return;
    }
    const auto u = model_.session().units();
    PatternBox::Values v;
    v.circular = true;
    v.center = h.id;
    v.angle = formatNumber(2 * std::numbers::pi / expr::angleFactor(u.angle));
    openPatternBox(v, {});
}

// «By line» ("Along a segment", §6.3, S4-SKT-006): while the button is pressed, a click on a sketch segment sets
// the pattern axis; a member of the edited pattern is not allowed.
void SketchEditor::clickPatternLine(const QPointF& s) {
    if (!patternBox_->isVisible() || (!patternBox_->pickingLine() && !patternBox_->pickingLine2())) return;
    const Hit h = hitAt(s);
    const auto* e = h.kind == Hit::Kind::Curve ? data()->findEntity(h.id) : nullptr;
    const auto* g = e ? data()->groupOfMember(e->id) : nullptr;
    if (!e || e->type != sketch::EntityType::Line || (g && g->id == patternEditing_)) {
        emit hint(txt("ui.sketch.requiredSketchLine"));
        return;
    }
    if (patternBox_->pickingLine2()) patternBox_->setLine2(e->id);  // «Направление 2» by its segment (FX12-FEAT-001)
    else patternBox_->setLine(e->id);
    emit hint("");
    view_.update();
}

void SketchEditor::openPatternBox(const PatternBox::Values& v, const std::string& editing) {
    patternEditing_ = editing;
    patternBox_->open(cursor_.toPoint(), v, !editing.empty());
    emit hint("");
    view_.update();
}

void SketchEditor::patternAccepted() {
    auto& s = model_.session();
    const doc::Json params = patternBox_->params();
    const bool circular = patternBox_->values().circular;
    Status st = okStatus();
    s.beginGroup(circular ? "undo.group.sketchPatternCircular" : "undo.group.sketchPatternLinear");
    if (patternEditing_.empty()) {
        auto r = s.sketchGroupCreate(patternBox_->type(), pending_, params);
        if (!r) st = r.error();
    } else {
        st = s.sketchGroupUpdate(patternEditing_, pending_, params);
    }
    s.endGroup();
    if (!st) {
        if (const auto* n = st.error().stringParam("name")) patternBox_->markError(*n);
        emit error(errorText(st.error(), s.document().document.get()));
        return;  // the dialog stays open (§6.3)
    }
    patternClosed();
    refresh();
}

void SketchEditor::patternClosed() {
    patternBox_->hide();
    patternEditing_.clear();
    pending_.clear();
    setTool(Tool::Select);
    view_.setFocus();
}

bool SketchEditor::selectionHasGroupMember() const {
    const auto* d = data();
    if (!d) return false;
    for (const auto& id : selection_)
        if (d->groupOfMember(id)) return true;
    return false;
}

void SketchEditor::unlinkSelection() {
    const auto* d = data();
    if (!d) return;
    std::vector<std::string> groups;
    for (const auto& id : selection_)
        if (const auto* g = d->groupOfMember(id); g && std::find(groups.begin(), groups.end(), g->id) == groups.end()) groups.push_back(g->id);
    if (groups.empty()) {
        emit hint(txt("ui.sketch.selectAPatternElement"));
        return;
    }
    auto& s = model_.session();
    s.beginGroup("undo.group.sketchUnlink");
    for (const auto& g : groups)
        if (auto r = s.sketchGroupUnlink(g); !r) emit error(errorText(r.error(), s.document().document.get()));
    s.endGroup();
    refresh();
}

std::vector<std::vector<QPointF>> SketchEditor::patternPreview() const {
    std::vector<std::vector<QPointF>> out;
    const auto* d = data();
    if (!patternBox_->isVisible() || !d) return out;
    auto items = api::sketchops::groupItems(*d, patternBox_->type(), pending_, patternBox_->params(), model_.session().units(),
                                            model_.session().variableValues().env());
    if (!items) return out;
    std::map<std::string, Point2> pts;  // "@role" → position
    for (const auto& it : *items)
        if (it.entity && it.entity->type == sketch::EntityType::Point) pts["@" + it.role] = Point2(it.entity->x, it.entity->y);
    auto P = [&](const std::string& ref) { return pts.count(ref) ? pts.at(ref) : Point2(0, 0); };
    for (const auto& it : *items) {
        if (!it.entity) continue;
        const auto& e = *it.entity;
        std::vector<QPointF> pl;
        if (e.type == sketch::EntityType::Line) {
            pl = {toScreen(P(e.p1)), toScreen(P(e.p2))};
        } else if (e.type == sketch::EntityType::Circle || e.type == sketch::EntityType::Arc) {
            const Point2 c = P(e.center);
            double r = e.r, a0 = 0, sw = 2 * std::numbers::pi;
            if (e.type == sketch::EntityType::Arc) {
                const Point2 s0 = P(e.start) - c, s1 = P(e.end) - c;
                r = s0.norm();
                a0 = std::atan2(s0.y(), s0.x());
                sw = geom2::ccwSweepPositive(a0, std::atan2(s1.y(), s1.x()));
            }
            for (int i = 0; i <= 64; ++i) pl.push_back(toScreen(c + r * Point2(std::cos(a0 + sw * i / 64), std::sin(a0 + sw * i / 64))));
        } else if (e.type == sketch::EntityType::Ellipse) {  // FX7-SK-012
            const Point2 c = P(e.center), ax(std::cos(e.angle), std::sin(e.angle)), ay(-ax.y(), ax.x());
            for (int i = 0; i <= 64; ++i) {
                const double t = 2 * std::numbers::pi * i / 64;
                pl.push_back(toScreen(c + e.rx * std::cos(t) * ax + e.ry * std::sin(t) * ay));
            }
        } else if (e.type == sketch::EntityType::Point) {
            // Cross marker only for copies of standalone points (curve points are visible via their curves).
            const auto slash = it.role.find('/');
            if (slash == std::string::npos || std::find(pending_.begin(), pending_.end(), it.role.substr(slash + 1)) == pending_.end()) continue;
            const QPointF q = toScreen(Point2(e.x, e.y));
            pl = {q + QPointF(-3, 0), q + QPointF(3, 0)};
        }
        if (!pl.empty()) out.push_back(std::move(pl));
    }
    return out;
}

}  // namespace construe::app
