// Reference geometry panel (spec/app-ux.md §5.3а).
#include "app/feature_panel.h"
#include "app/flip_button.h"

#include <QStandardItemModel>

#include "app/app_model.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <numbers>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace construe::app {

// ---------------------------------------------------------------- reference geometry (§5.3а)

namespace {

struct ModeDef {
    const char* mode;
    const char* label;
};
// «Up to object» and «At distance from object» (FX4-FEAT-001) are variants
// of an offset plane: in the document, mode "offset" with "to".
const ModeDef PLANE_MODES[] = {{"offset", "ui.common.offset2"},
                               {"offsetTo", "ui.feature.offsetUpToObject"},
                               {"offsetFrom", "ui.feature.offsetByDistanceFromObject"},
                               {"angle", "ui.common.atAngle"},
                               {"threePoints", "ui.common.throughThreePoints"},
                               {"mid", "ui.common.midplane"}};
const ModeDef AXIS_MODES[] = {{"twoPoints", "ui.common.throughTwoPoints"}, {"edge", "ui.common.alongEdge"}, {"face", "ui.common.faceAxis"}, {"planes", "ui.common.intersectionOfPlanes"}};

// Reference kinds that a view object can become (§5.3а).
std::vector<std::string> kindsOf(const PickTarget& t) {
    switch (t.kind) {
        case PickTarget::Kind::Plane: return {"plane"};
        case PickTarget::Kind::Face: return {"plane", "face", "target"};
        case PickTarget::Kind::Edge: return {"line", "edge", "target"};
        case PickTarget::Kind::Axis: return {"line"};
        case PickTarget::Kind::Vertex:
        case PickTarget::Kind::Origin: return {"point", "target"};
        default: return {};
    }
}

doc::Json refJson(const PickTarget& t, const std::string& kind) {
    if (kind == "plane") {
        if (t.kind == PickTarget::Kind::Face) return {{"face", t.name}};
        return t.name.rfind("plane:", 0) == 0 ? doc::Json{{"datum", t.name}} : doc::Json{{"feature", t.name}};
    }
    if (kind == "line") {
        if (t.kind == PickTarget::Kind::Edge) return {{"edge", t.name}};
        return t.name.rfind("axis:", 0) == 0 ? doc::Json{{"datum", t.name}} : doc::Json{{"feature", t.name}};
    }
    if (kind == "point") return t.kind == PickTarget::Kind::Origin ? doc::Json{{"datum", "origin"}} : doc::Json{{"vertex", t.name}};
    if (kind == "target") {  // object of an "up to object" plane (TargetRef, FX4-FEAT-001)
        if (t.kind == PickTarget::Kind::Face) return {{"face", t.name}};
        if (t.kind == PickTarget::Kind::Edge) return {{"edge", t.name}};
        return t.kind == PickTarget::Kind::Origin ? doc::Json{{"datum", "origin"}} : doc::Json{{"vertex", t.name}};
    }
    return t.name;  // face / edge: the name
}

PickTarget targetOf(const doc::Json& r, const std::string& kind) {
    if (r.is_string()) return {kind == "face" ? PickTarget::Kind::Face : PickTarget::Kind::Edge, {}, r.get<std::string>()};
    if (r.contains("face")) return {PickTarget::Kind::Face, {}, r["face"]};
    if (r.contains("edge")) return {PickTarget::Kind::Edge, {}, r["edge"]};
    if (r.contains("vertex")) return {PickTarget::Kind::Vertex, {}, r["vertex"]};
    if (r.contains("datum")) {
        const std::string d = r["datum"];
        if (d == "origin") return {PickTarget::Kind::Origin, {}, "origin"};
        return {d.rfind("axis:", 0) == 0 ? PickTarget::Kind::Axis : PickTarget::Kind::Plane, {}, d};
    }
    return {kind == "line" ? PickTarget::Kind::Axis : PickTarget::Kind::Plane, {}, r.value("feature", std::string())};
}

}  // namespace

RefGeomPanel::RefGeomPanel(AppModel& model, bool plane, QWidget* parent)
    : FeaturePanel(model, plane ? txt("ui.common.plane") : txt("ui.common.axis"), parent, Profiles::Hidden), plane_(plane) {
    setObjectName(plane ? "refPlanePanel" : "refAxisPanel");
    mode_ = new QComboBox;
    mode_->setObjectName("refMode");
    for (const auto& m : plane ? std::vector<ModeDef>(std::begin(PLANE_MODES), std::end(PLANE_MODES))
                               : std::vector<ModeDef>(std::begin(AXIS_MODES), std::end(AXIS_MODES)))
        mode_->addItem(QString::fromUtf8(txt(m.label)), QString(m.mode));
    form_->addRow(txt("ui.common.type"), mode_);
    refsLabel_ = addSelectionField("refs", txt("ui.feature.references"), txt("ui.feature.selectReferences"), [this](int i) {
        for (auto& r : refs_)
            if (r && i-- == 0) r.reset();
    });
    refsLabel_->setRequired(true);
    refItems_ = refsLabel_->items();
    auto* row = new QHBoxLayout;
    value_ = newDimensionField("distance");  // offset distance or angle by mode (setModeKeepRefs)
    value_->setObjectName("refValue");
    flip_ = new FlipButton("refFlip");
    row->addWidget(dimRow(value_), 1);
    row->addWidget(flip_);
    form_->addRow(txt("ui.common.value"), row);
    valueLabel_ = form_->labelForField(row);
    far_ = new QCheckBox(txt("ui.feature.farTangent"));
    far_->setObjectName("refFar");
    far_->setToolTip(txt("ui.feature.ofTheTwoTangentsTo"));
    form_->addRow(QString(), far_);
    connect(far_, &QCheckBox::toggled, this, &RefGeomPanel::changed);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { setModeKeepRefs(mode()); });
    connect(value_, &QLineEdit::textEdited, this, &RefGeomPanel::changed);
    connect(value_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    connect(flip_, &QToolButton::toggled, this, &RefGeomPanel::changed);
}

std::string RefGeomPanel::mode() const { return mode_->currentData().toString().toStdString(); }

bool RefGeomPanel::toMode() const {
    const std::string m = mode();
    return m == "offsetTo" || m == "offsetFrom";
}

std::vector<std::string> RefGeomPanel::slotKinds() const {
    const std::string m = mode();
    if (m == "offset") return {"plane"};
    if (m == "offsetTo" || m == "offsetFrom") return {"plane", "target"};
    if (m == "angle") return {"plane", "line"};
    if (m == "threePoints") return {"point", "point", "point"};
    if (m == "mid" || m == "planes") return {"plane", "plane"};
    if (m == "twoPoints") return {"point", "point"};
    if (m == "edge") return {"edge"};
    return {"face"};
}

void RefGeomPanel::setModeKeepRefs(const std::string&) {
    // References that suit the new mode are kept in order (§5.3а).
    std::vector<Ref> old;
    for (const auto& r : refs_)
        if (r) old.push_back(*r);
    std::sort(old.begin(), old.end(), [](const Ref& a, const Ref& b) { return a.order < b.order; });
    refs_.assign(slotKinds().size(), std::nullopt);
    for (const auto& r : old) pick(r.target);
    const std::string m = mode();
    const bool hasValue = m == "offset" || m == "offsetFrom" || m == "angle";
    dimRow(value_)->setVisible(hasValue);
    DimensionField::of(value_)->setParam(m == "angle" ? "angle" : "distance");
    if (valueLabel_) valueLabel_->setVisible(hasValue);
    flip_->setVisible(m == "offset" || m == "offsetFrom");
    far_->setVisible(toMode());
    const auto d = model_.session().document().document;
    if ((m == "offset" || m == "offsetFrom") && value_->property("dim").toString() != "length") {
        value_->setText(formatNumber(10 / expr::lengthFactor(model_.session().units().length)));  // REF_PLANE_DEFAULT_OFFSET = 10 mm
        value_->setProperty("dim", "length");
    } else if (m == "angle" && value_->property("dim").toString() != "angle") {
        value_->setText(formatNumber(45 / expr::angleFactor(model_.session().units().angle) * std::numbers::pi / 180));  // REF_PLANE_DEFAULT_ANGLE = 45°
        value_->setProperty("dim", "angle");
    }
    changed();
}

void RefGeomPanel::startNew(const std::optional<std::string>&) { startNewMode(plane_ ? "offset" : "twoPoints"); }

void RefGeomPanel::startNewMode(const std::string& m) {
    loadTolerances(doc::Json::object());  // a new feature — no tolerances (FX12-PMI-007)
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    refs_.clear();
    pickCounter_ = 0;
    flip_->setChecked(false);
    far_->setChecked(false);
    value_->setProperty("dim", QString());
    const QSignalBlocker b(mode_);
    mode_->setCurrentIndex(std::max(0, mode_->findData(QString::fromStdString(m))));
    setModeKeepRefs(m);
}

void RefGeomPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || !f->refGeom()) return;
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    banner_->hide();
    loadDefinition(f->refGeom()->def);
}

void RefGeomPanel::setOffsetText(const QString& t) {
    value_->setText(t);
    changed();
}

void RefGeomPanel::startNewFrom(const doc::Json& def) {
    loadTolerances(doc::Json::object());
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    flip_->setChecked(false);
    far_->setChecked(false);
    value_->setProperty("dim", QString());
    loadDefinition(def);
}

void RefGeomPanel::loadDefinition(const doc::Json& def) {
    const auto d = model_.session().document().document;
    // Offset relative to an object (FX4-FEAT-001): "up to object" has distance "0", else "at distance from object".
    std::string m = def["mode"];
    if (m == "offset" && def.contains("to")) m = def.value("distance", std::string("0")) == "0" ? "offsetTo" : "offsetFrom";
    {
        const QSignalBlocker b(mode_);
        mode_->setCurrentIndex(std::max(0, mode_->findData(QString::fromStdString(m))));
    }
    const auto kinds = slotKinds();
    refs_.assign(kinds.size(), std::nullopt);
    pickCounter_ = 0;
    auto put = [&](size_t i, const doc::Json& j) {
        if (i < refs_.size()) refs_[i] = Ref{kinds[i], j, targetOf(j, kinds[i]), ++pickCounter_};
    };
    if (def.contains("ref")) put(0, def["ref"]);
    if (def.contains("to")) put(1, def["to"]);
    if (def.contains("axis")) put(1, def["axis"]);
    for (const char* key : {"refs", "points"})
        if (def.contains(key))
            for (size_t i = 0; i < def[key].size(); ++i) put(i, def[key][i]);
    if (def.contains("edge")) put(0, def["edge"]);
    if (def.contains("face")) put(0, def["face"]);
    flip_->setChecked(def.value("flip", false));
    far_->setChecked(def.value("far", false));
    if (def.contains("distance")) value_->setText(displayExpr(def["distance"].get<std::string>(), d->lengthUnit)), value_->setProperty("dim", "length");
    if (def.contains("angle") && def["angle"].is_string()) value_->setText(displayExpr(def["angle"].get<std::string>(), d->angleUnit)), value_->setProperty("dim", "angle");
    setModeKeepRefs(m);
}

FeaturePanel::PickOutcome RefGeomPanel::pickTree(const TreePick& p) {
    // Planes, axes, the origin and reference features — into «References»; other nodes are ignored (§5.3а).
    if (!p.target) return PickOutcome::Taken;
    return pick(*p.target) ? PickOutcome::Taken : PickOutcome::Refused;
}

bool RefGeomPanel::pick(const PickTarget& t) {
    // A repeated click on a selected reference removes it.
    for (auto& r : refs_)
        if (r && r->target.kind == t.kind && r->target.name == t.name) {
            r.reset();
            changed();
            return true;
        }
    const auto cand = kindsOf(t);
    const auto kinds = slotKinds();
    auto fits = [&](const std::string& k) {
        if (std::find(cand.begin(), cand.end(), k) == cand.end()) return false;
        // In "up to object" modes a non-planar face is the object (tangent), not the source plane (FX4-FEAT-001).
        if (k == "plane" && t.kind == PickTarget::Kind::Face && toMode()) {
            const auto info = model_.session().topologyInfo(t.name, api::StateRef::final());
            return info && info->surface && info->surface->kind == kernel::SurfaceInfo::Kind::Plane;
        }
        return true;
    };
    int slot = -1;
    for (size_t i = 0; i < kinds.size() && slot < 0; ++i)
        if (!refs_[i] && fits(kinds[i])) slot = int(i);
    if (slot < 0) {  // slots are full: the earliest reference of the same kind is replaced
        int best = -1;
        for (size_t i = 0; i < kinds.size(); ++i)
            if (fits(kinds[i]) && (best < 0 || refs_[i]->order < refs_[size_t(best)]->order)) best = int(i);
        slot = best;
    }
    if (slot < 0) return false;
    const std::string k = kinds[size_t(slot)];
    refs_[size_t(slot)] = Ref{k, refJson(t, k), t, ++pickCounter_};
    changed();
    return true;
}

QString RefGeomPanel::needed() const {
    const std::string m = mode();
    if (m == "offset") return txt("ui.feature.requiredPlaneOrPlanarFace");
    if (m == "offsetTo" || m == "offsetFrom") return txt("ui.feature.requiredPlaneAndObjectVertex");
    if (m == "angle") return txt("ui.feature.requiredPlaneAndAxisStraight");
    if (m == "threePoints" || m == "twoPoints") return txt("ui.feature.requiredVertexOrOrigin");
    if (m == "edge") return txt("ui.feature.requiredStraightEdge");
    if (m == "face") return txt("ui.feature.requiredCylindricalOrConicalFace");
    return txt("ui.feature.requiredPlaneOrPlanarFace");
}

std::vector<PickTarget> RefGeomPanel::selectedTargets() const {
    std::vector<PickTarget> out;
    for (const auto& r : refs_)
        if (r) out.push_back(r->target);
    return out;
}

bool RefGeomPanel::canAccept() const {
    for (const auto& r : refs_)
        if (!r) return false;
    return !refs_.empty();
}

void RefGeomPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    // Captions of the list style (app-ux §5.5г): «Face N», «Edge N», «Vertex», «Front», a feature name.
    std::vector<SelectionItem> items;
    for (const auto& r : refs_)
        if (r) items.push_back({r->target.kind == PickTarget::Kind::Edge     ? topoCaption(model_.session(), r->target.name, true)
                                : r->target.kind == PickTarget::Kind::Face   ? topoCaption(model_.session(), r->target.name, false)
                                : r->target.kind == PickTarget::Kind::Vertex ? QString(txt("ui.feature.vertex"))
                                                                             : objectCaption(model_.session(), r->target.name, CaptionStyle::List),
                                {r->target}, {}});
    refsLabel_->setSelection(std::move(items));
    const auto d = model_.session().document().document;
    if (mode() == "offset" || mode() == "offsetFrom") showExprState(value_, expr::LENGTH);
    else if (mode() == "angle") showExprState(value_, expr::ANGLE);
}

doc::Json RefGeomPanel::definition() const {
    const auto d = model_.session().document().document;
    const std::string m = mode();
    doc::Json def = {{"type", plane_ ? "plane" : "axis"}, {"mode", toMode() ? std::string("offset") : m}};
    auto ref = [&](size_t i) { return i < refs_.size() && refs_[i] ? refs_[i]->json : doc::Json(nullptr); };
    if (toMode()) {  // offset up to object / at distance from object (FX4-FEAT-001)
        def["ref"] = ref(0);
        def["to"] = ref(1);
        def["distance"] = m == "offsetTo" ? std::string("0") : value_->expression();
        if (m == "offsetFrom" && flip_->isChecked()) def["flip"] = true;
        if (far_->isChecked()) def["far"] = true;
    } else if (m == "offset") {
        def["ref"] = ref(0);
        def["distance"] = value_->expression();
        if (flip_->isChecked()) def["flip"] = true;
    } else if (m == "angle") {
        def["ref"] = ref(0);
        def["axis"] = ref(1);
        def["angle"] = value_->expression();
    } else if (m == "threePoints" || m == "twoPoints") {
        def["points"] = doc::Json::array();
        for (size_t i = 0; i < refs_.size(); ++i) def["points"].push_back(ref(i));
    } else if (m == "mid" || m == "planes") {
        def["refs"] = doc::Json::array({ref(0), ref(1)});
    } else if (m == "edge") {
        def["edge"] = ref(0);
    } else {
        def["face"] = ref(0);
    }
    return def;
}

}  // namespace construe::app
