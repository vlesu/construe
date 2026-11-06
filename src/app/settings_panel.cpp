// "Document settings" panel (app/settings_panel.h).
#include "app/panel_header.h"
#include "app/messages.h"
#include "app/settings_panel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <utility>

namespace construe::app {

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("settingsPanel");
    auto* sv = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(txt("ui.docSettings.documentSettings"), PanelHeader::CommandButtons{"settingsOk", "settingsCancel"});
    sv->addWidget(head);
    auto* form = new QFormLayout;
    length_ = new QComboBox;
    length_->setObjectName("settingsLength");
    for (const auto& [t, u] : {std::pair{txt("ui.common.mm2"), "mm"}, {txt("ui.common.cm"), "cm"}, {txt("ui.common.m"), "m"}, {txt("ui.docSettings.in"), "in"}, {txt("ui.docSettings.ft"), "ft"}})
        length_->addItem(QString::fromUtf8(t), QString(u));
    form->addRow(txt("ui.docSettings.lengthUnits"), length_);
    angle_ = new QComboBox;
    angle_->setObjectName("settingsAngle");
    angle_->addItem(txt("ui.docSettings.degrees"), "deg");
    angle_->addItem(txt("ui.docSettings.radians"), "rad");
    form->addRow(txt("ui.docSettings.angleUnits"), angle_);
    sv->addLayout(form);
    sv->addStretch(1);
    connect(head, &PanelHeader::accepted, this, &SettingsPanel::accepted);
    connect(head, &PanelHeader::cancelled, this, &SettingsPanel::cancelled);
}

void SettingsPanel::load(const std::string& lengthUnit, const std::string& angleUnit) {
    length_->setCurrentIndex(std::max(0, length_->findData(QString::fromStdString(lengthUnit))));
    angle_->setCurrentIndex(std::max(0, angle_->findData(QString::fromStdString(angleUnit))));
}

std::string SettingsPanel::lengthUnit() const { return length_->currentData().toString().toStdString(); }
std::string SettingsPanel::angleUnit() const { return angle_->currentData().toString().toStdString(); }

QString unitsStatusText(const std::string& lengthUnit, const std::string& angleUnit) {
    const std::map<std::string, QString> names = {{"mm", txt("ui.common.mm2")}, {"cm", txt("ui.common.cm")}, {"m", txt("ui.common.m")}, {"in", txt("ui.docSettings.in")}, {"ft", txt("ui.docSettings.ft")}};
    const auto it = names.find(lengthUnit);
    return (it != names.end() ? it->second : QString::fromStdString(lengthUnit)) + ", " +
           (angleUnit == "rad" ? QString(txt("ui.docSettings.rad")) : QString::fromUtf8("°"));
}

}  // namespace construe::app
