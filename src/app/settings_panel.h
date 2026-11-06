// "Document settings" panel (app-ux §10.4): length and angle units.
#pragma once

#include <QComboBox>
#include <QString>
#include <QWidget>

#include <string>

namespace construe::app {

class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget* parent = nullptr);
    void load(const std::string& lengthUnit, const std::string& angleUnit);
    std::string lengthUnit() const;
    std::string angleUnit() const;

signals:
    void accepted();
    void cancelled();

private:
    QComboBox* length_ = nullptr;
    QComboBox* angle_ = nullptr;
};

// Document units in the status bar (§2.1): «mm, °», «inch, rad».
QString unitsStatusText(const std::string& lengthUnit, const std::string& angleUnit);

}  // namespace construe::app
