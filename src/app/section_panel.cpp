#include "app/section_panel.h"
#include "app/panel_header.h"

#include "app/app_model.h"
#include "app/captions.h"
#include "app/choice_row.h"
#include "app/feature_panel.h"
#include "app/flip_button.h"
#include "app/messages.h"
#include "app/selection_field_widget.h"
#include "app/theme.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace construe::app {

SectionPanel::SectionPanel(AppModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("sectionPanel");
    auto* col = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.common.section"), PanelHeader::CommandButtons{"sectionOk", "sectionCancel"});
    col->addWidget(head);
    auto* kinds = new ChoiceRow({{"plane", txt("ui.section.byPlane"), "sectionKindPlane"}, {"sketch", txt("ui.section.bySketch"), "sectionKindSketch"}});
    kindPlane_ = kinds->button("plane");
    kindSketch_ = kinds->button("sketch");
    col->addWidget(kinds);
    planeRow_ = new QWidget;
    auto* pf = new QFormLayout(planeRow_);
    pf->setContentsMargins(0, 0, 0, 0);
    planeField_ = new SelectionFieldWidget("sectionPlane", txt("ui.section.clickAPlaneOrPlanar"), this);
    planeField_->setRequired(true);
    planeField_->addTo(pf, txt("ui.common.plane"));
    connect(planeField_, &SelectionFieldWidget::removed, this, [this](int) {  // ✕ empties the field (§5.5)
        plane_ = nullptr;
        syncPlane();
        emit changed();
    });
    offset_ = new ExprField;
    offset_->setChecker(&model_.session(), expr::LENGTH);
    offset_->setText("0");
    offset_->setObjectName("sectionOffset");
    pf->addRow(txt("ui.common.offset"), offset_);
    col->addWidget(planeRow_);
    sketchRow_ = new QWidget;
    auto* sf = new QFormLayout(sketchRow_);
    sf->setContentsMargins(0, 0, 0, 0);
    sketch_ = new QComboBox;
    sketch_->setObjectName("sectionSketch");
    sf->addRow(txt("ui.common.sketch"), sketch_);
    col->addWidget(sketchRow_);
    auto* flipRow = new QHBoxLayout;  // «Flip» ⇅ (app-ux §5.13)
    flip_ = new FlipButton("sectionFlip", txt("ui.section.cutTheOtherSide"));
    flipRow->addWidget(new QLabel(txt("ui.common.flip")));
    flipRow->addWidget(flip_);
    flipRow->addStretch(1);
    col->addLayout(flipRow);
    excludeLabel_ = new QLabel(txt("ui.section.doNotCut"));
    col->addWidget(excludeLabel_);
    exclude_ = new CheckList("sectionExclude");
    exclude_->setMaximumHeight(120);
    col->addWidget(exclude_);
    connect(exclude_, &CheckList::changed, this, [this] { emit changed(); });
    status_ = new QLabel;
    status_->setObjectName("sectionStatus");
    status_->setWordWrap(true);
    status_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    col->addWidget(status_);
    col->addStretch(1);
    connect(kindPlane_, &QAbstractButton::toggled, this, [this] {
        syncKind();
        emit changed();
    });
    connect(offset_, &QLineEdit::textEdited, this, [this] { emit changed(); });
    connect(sketch_, &QComboBox::activated, this, [this] { emit changed(); });
    connect(flip_, &QToolButton::toggled, this, [this] { emit changed(); });
    connect(head, &PanelHeader::accepted, this, &SectionPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &SectionPanel::cancelled);
    head->acceptOnEnter(this);  // Enter in a field — ✓ (§2.1д)
}

void SectionPanel::syncKind() {
    planeRow_->setVisible(kindPlane_->isChecked());
    sketchRow_->setVisible(!kindPlane_->isChecked());
}

void SectionPanel::start(const std::optional<std::string>& editing, const doc::Json& def) {
    editing_ = editing;
    const auto d = model_.session().document().document;
    {
        const QSignalBlocker b1(kindPlane_), b2(flip_), b3(sketch_);
        sketch_->clear();
        for (const auto& feat : d->active().features)
            if (feat->sketchData()) sketch_->addItem(QString::fromStdString(feat->name), QString::fromStdString(feat->id));
        const bool plane = !def.contains("sketch");
        kindPlane_->setChecked(plane);
        kindSketch_->setChecked(!plane);
        flip_->setChecked(def.value("flip", false));
        offset_->setText(QString::fromStdString(def.value("offset", std::string("0"))));
        plane_ = def.contains("plane") ? def["plane"] : doc::Json{{"datum", "plane:front"}};
        if (def.contains("sketch")) sketch_->setCurrentIndex(sketch_->findData(QString::fromStdString(def["sketch"].get<std::string>())));
        // Assembly: list of top-level instances with "do not cut" checkboxes.
        const QSignalBlocker b4(exclude_);
        exclude_->clear();
        const bool asmEl = d->active().isAssembly();
        assembly_ = asmEl;
        excludeLabel_->setVisible(asmEl);
        exclude_->setVisible(asmEl);
        kindSketch_->setVisible(true);  // for an assembly — the assembly's sketches (S2-ASM-012)
        std::vector<CheckList::Item> rows;
        if (asmEl)
            for (const auto& in : d->active().instances) {
                bool ex = false;
                if (def.contains("exclude"))
                    for (const auto& x : def["exclude"]) ex = ex || x.get<std::string>() == in.id;
                rows.push_back({QString::fromStdString(in.id), QString::fromStdString(in.name), ex});
            }
        exclude_->setItems(rows);
    }
    syncPlane();
    syncKind();
    status_->clear();
}

doc::Json SectionPanel::definition() const {
    doc::Json def = doc::Json::object();
    if (kindPlane_->isChecked()) {
        if (!plane_.is_null()) def["plane"] = plane_;
        const auto d = model_.session().document().document;
        std::string off = offset_->expression();
        def["offset"] = off.empty() ? std::string("0") : off;
    } else {
        def["sketch"] = sketch_->currentData().toString().toStdString();
    }
    if (flip_->isChecked()) def["flip"] = true;
    doc::Json ex = doc::Json::array();
    for (const auto& id : exclude_->checkedIds()) ex.push_back(id);
    if (!ex.empty() && assembly_) def["exclude"] = ex;
    return def;
}

void SectionPanel::setPlane(const doc::Json& ref) {
    plane_ = ref;
    syncPlane();
    emit changed();
}

void SectionPanel::syncPlane() {
    std::vector<SelectionItem> items;
    const auto& s = model_.session();
    if (plane_.is_object() && plane_.contains("datum")) {
        const std::string id = plane_["datum"];
        items.push_back({objectCaption(s, id, CaptionStyle::List), {{PickTarget::Kind::Plane, {}, id}}, {}});
    } else if (plane_.is_object() && plane_.contains("feature")) {
        const std::string id = plane_["feature"];
        items.push_back({objectCaption(s, id, CaptionStyle::List), {{PickTarget::Kind::Plane, {}, id}}, {}});
    } else if (plane_.is_object() && plane_.contains("face")) {
        const std::string face = plane_["face"];
        if (face.find('/') != std::string::npos)  // a face of an assembly instance: the view resolves the reference
            items.push_back({objectCaption(s, face, CaptionStyle::Assembly, api::StateRef::final(), PickTarget::Kind::Face), {}, face});
        else
            items.push_back({topoCaption(s, face, false), {{PickTarget::Kind::Face, {}, face}}, {}});
    }
    planeField_->setSelection(std::move(items));
}

std::optional<double> SectionPanel::offsetMm() const {
    const auto d = model_.session().document().document;
    auto q = model_.session().checkExpression(offset_->expression(), expr::LENGTH);
    if (!q) return std::nullopt;
    return q->value;
}

void SectionPanel::setOffsetFromManipulator(double mm) {
    const auto u = model_.session().units();
    offset_->setText(formatNumber(mm / expr::lengthFactor(u.length)));  // number in document units (§5.6)
    emit changed();
}

void SectionPanel::setStatus(const QString& error) { status_->setText(error); }

}  // namespace construe::app
