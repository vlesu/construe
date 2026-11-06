// STEP, STL, 3MF import panel (app-ux §10.7): choice "as body" / "recognize construction" and the shape healing and
// dimension rounding tolerances for this import (S4-REC-018). Running the import belongs to the main window.
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QAbstractButton>
#include <QString>
#include <QWidget>

namespace construe::app {

class ImportPanel : public QWidget {
    Q_OBJECT
public:
    explicit ImportPanel(QWidget* parent = nullptr);
    // Opening the panel: file name, whether it is a mesh (healing only for STEP) and default tolerance values (mm)
    // from the application settings; choice — "as body" (rounding is available only with "recognize").
    void reset(const QString& file, bool mesh, double healTolerance, double roundTolerance);
    bool recognize() const { return recognize_->isChecked(); }
    double healTolerance() const { return heal_->currentData().toString().toDouble(); }
    double roundTolerance() const { return round_->currentData().toString().toDouble(); }

signals:
    void accepted();
    void cancelled();

private:
    void update();
    static void select(QComboBox* c, double mm);

    QLabel* file_ = nullptr;
    QAbstractButton* asBody_ = nullptr;  // options of the import row (ChoiceRow)
    QAbstractButton* recognize_ = nullptr;
    QComboBox* heal_ = nullptr;
    QComboBox* round_ = nullptr;
    QLabel* healLabel_ = nullptr;
    bool mesh_ = false;
};

}  // namespace construe::app
