// STEP, STL, 3MF import panel (app/import_panel.h).
#include "app/import_panel.h"
#include "app/panel_header.h"
#include "app/choice_row.h"

#include "app/app_settings.h"
#include "app/messages.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <cmath>

namespace construe::app {

ImportPanel::ImportPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("importPanel");
    auto* ev = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.common.import"), PanelHeader::CommandButtons{"importOk", "importCancel"});
    ev->addWidget(head);
    file_ = new QLabel;
    file_->setObjectName("importFile");
    file_->setWordWrap(true);
    ev->addWidget(file_);
    auto* how = new ChoiceRow({{"body", txt("ui.import.importAsBody"), "importAsBody"}, {"recognize", txt("ui.common.recognizeDesign"), "importRecognize"}},
                              nullptr, nullptr, true);
    asBody_ = how->button("body");
    recognize_ = how->button("recognize");
    ev->addWidget(how);
    auto* form = new QFormLayout;
    auto fill = [](QComboBox* c, const QStringList& values) {
        for (const auto& v : values) c->addItem(v == "0" ? QString(txt("ui.common.off")) : v + txt("ui.common.mm"), v);
    };
    heal_ = new QComboBox;
    heal_->setObjectName("importHeal");
    fill(heal_, HEAL_TOLERANCES);
    healLabel_ = new QLabel(txt("ui.import.shapeHealingTolerance"));
    form->addRow(healLabel_, heal_);
    round_ = new QComboBox;
    round_->setObjectName("importRound");
    fill(round_, ROUND_TOLERANCES);
    form->addRow(txt("ui.import.dimensionRounding"), round_);
    ev->addLayout(form);
    ev->addStretch(1);
    connect(asBody_, &QAbstractButton::toggled, this, &ImportPanel::update);
    connect(head, &PanelHeader::accepted, this, &ImportPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &ImportPanel::cancelled);
    update();
}

void ImportPanel::select(QComboBox* c, double mm) {
    // The settings value is always from the list (AppSettings validates); otherwise — the nearest one.
    int best = 0;
    for (int i = 0; i < c->count(); ++i)
        if (std::abs(c->itemData(i).toString().toDouble() - mm) < std::abs(c->itemData(best).toString().toDouble() - mm)) best = i;
    c->setCurrentIndex(best);
}

void ImportPanel::reset(const QString& file, bool mesh, double healTolerance, double roundTolerance) {
    mesh_ = mesh;
    file_->setText(file);
    asBody_->setChecked(true);
    select(heal_, healTolerance);
    select(round_, roundTolerance);
    update();
}

// Shape healing — only for STEP (a mesh has no entity tolerances) and for any choice (as before the panel, S4-REC-013);
// dimension rounding — only with "recognize".
void ImportPanel::update() {
    heal_->setEnabled(!mesh_);
    healLabel_->setEnabled(!mesh_);
    round_->setEnabled(recognize_->isChecked());
}

}  // namespace construe::app
