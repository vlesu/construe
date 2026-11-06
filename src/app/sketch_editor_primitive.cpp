// Sketcher: primitives — N-gon and star as linked groups, primitive dialog (spec/app-ux.md §6.3; S4-SKT-003).
#include "app/sketch_editor.h"

#include "api/sketch_ops.h"
#include "app/app_model.h"
#include "app/messages.h"

#include <cmath>
#include <numbers>

namespace construe::app {

namespace {
constexpr double STAR_INNER_RATIO = 0.5;  // app-ux §11: default star valley diameter
}  // namespace

void SketchEditor::setStarRays(int n) {
    starRays_ = std::clamp(n, 3, 40);
    view_.update();
    emit statusChanged();
}

bool SketchEditor::createPrimitive(const char* undoKey, const std::string& type, const doc::Json& params,
                                   const std::vector<std::pair<std::string, Snap>>& snaps) {
    auto& s = model_.session();
    s.beginGroup(undoKey);
    auto g = s.sketchGroupCreate(type, {}, params);
    bool ok = g.ok();
    if (!ok) emit error(errorText(g.error(), s.document().document.get()));
    if (ok) {
        // Click snaps become constraints on group members (role → ID), "weak" ones (§6.4).
        doc::Json items = doc::Json::array();
        if (const auto* d = data())
            if (const auto* grp = d->findGroup(*g))
                for (const auto& [role, snap] : snaps)
                    if (const auto* m = grp->member(role)) snapConstraint(items, m->id, snap);
        int next = 1;
        std::vector<std::string> autoTemp;
        for (const auto& it : items)
            if (it.contains("id")) next = std::max(next, std::stoi(it["id"].get<std::string>().substr(1)) + 1);
        for (auto& it : items)
            if (!it.contains("id")) it["id"] = "t" + std::to_string(next++), autoTemp.push_back(it["id"]);
        if (!items.empty()) {
            auto r = s.sketchAdd(items);
            if (!r) {
                emit error(errorText(r.error(), s.document().document.get()));
            } else {
                std::vector<std::string> autoIds;
                for (const auto& t : autoTemp)
                    if (auto it = r->find(t); it != r->end()) autoIds.push_back(it->second);
                for (int guard = 0; guard < 64 && !autoIds.empty(); ++guard) {
                    const auto victim = s.sketchYieldAuto(autoIds);
                    if (!victim || victim->empty()) break;
                    std::erase(autoIds, *victim);
                }
            }
        }
    }
    s.endGroup();
    refresh();
    return ok;
}

void SketchEditor::clickPolygon(const QPointF& sp, bool suppress) {
    const Snap sn = snapAt(sp, suppress);
    if (clicks_.empty()) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 c = clicks_[0];
    if ((toScreen(sn.at) - toScreen(c)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    const Point2 v = sn.at - c;
    const doc::Json params = {{"sides", polygonSides_}, {"inscribed", polygonInscribed_}, {"x", c.x()}, {"y", c.y()},
                              {"size", v.norm()}, {"angle", std::atan2(v.y(), v.x())}};
    std::vector<std::pair<std::string, Snap>> snaps = {{"center", clickSnaps_[0]}};
    if (polygonInscribed_) snaps.push_back({"side0/p1", sn});  // the vertex gets all snaps
    createPrimitive("undo.group.polygon", "polygon", params, snaps);
    resetToolState();
}

void SketchEditor::clickStar(const QPointF& sp, bool suppress) {
    const Snap sn = snapAt(sp, suppress);
    if (clicks_.empty()) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 c = clicks_[0];
    if ((toScreen(sn.at) - toScreen(c)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    const Point2 v = sn.at - c;
    const double d1 = 2 * v.norm();
    const doc::Json params = {{"rays", starRays_}, {"d1", lengthText(d1)}, {"d2", lengthText(d1 * STAR_INNER_RATIO)},
                              {"x", c.x()}, {"y", c.y()}, {"angle", std::atan2(v.y(), v.x())}};
    createPrimitive("undo.group.star", "star", params, {{"center", clickSnaps_[0]}});
    resetToolState();
}

bool SketchEditor::openPrimitiveBox(const std::string& memberId, const QPointF& at) {
    const auto* d = data();
    const auto* g = d ? d->groupOfMember(memberId) : nullptr;
    if (!g || (g->type != "polygon" && g->type != "star")) return false;
    PrimitiveBox::Values v;
    v.star = g->type == "star";
    auto num = [&](const char* k, double def) {
        const auto it = g->params.find(k);
        return it == g->params.end() ? def : it->second.number;
    };
    auto txt = [&](const char* k) {
        const auto it = g->params.find(k);
        return it == g->params.end() ? QString() : QString::fromStdString(it->second.text);
    };
    v.count = int(num(v.star ? "rays" : "sides", 6));
    if (const auto it = g->params.find("inscribed"); it != g->params.end()) v.inscribed = it->second.flag;
    v.d1 = txt("d1"), v.d2 = txt("d2");
    setTool(Tool::Select);
    primitiveEditing_ = g->id;
    primitiveBox_->open(at.toPoint(), v);
    return true;
}

void SketchEditor::primitiveAccepted() {
    const auto* d = data();
    const auto* g = d ? d->findGroup(primitiveEditing_) : nullptr;
    if (!g) {
        primitiveClosed();
        return;
    }
    const PrimitiveBox::Values v = primitiveBox_->values();
    doc::Json params = doc::Json::object();
    for (const auto& [k, p] : g->params) {  // previous parameters (initial position — §2.6.4)
        if (p.kind == sketch::GroupParam::Kind::Number) params[k] = p.number;
        else if (p.kind == sketch::GroupParam::Kind::Flag) params[k] = p.flag;
        else params[k] = p.text;
    }
    if (v.star) {
        params["rays"] = v.count;
        params["d1"] = v.d1.toStdString();
        params["d2"] = v.d2.toStdString();
    } else {
        params["sides"] = v.count;
        params["inscribed"] = v.inscribed;
    }
    auto& s = model_.session();
    s.beginGroup("undo.group.primitive");
    auto st = s.sketchGroupUpdate(primitiveEditing_, {}, params);
    s.endGroup();
    if (!st) {
        if (const auto* n = st.error().stringParam("name")) primitiveBox_->markError(*n);
        emit error(errorText(st.error(), s.document().document.get()));
        return;  // the dialog stays open (§6.3)
    }
    primitiveClosed();
    refresh();
}

void SketchEditor::primitiveClosed() {
    primitiveBox_->hide();
    primitiveEditing_.clear();
    view_.setFocus();
}

}  // namespace construe::app
