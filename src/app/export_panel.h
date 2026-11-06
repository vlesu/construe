// STL, 3MF and flat pattern DXF export panel (app-ux §10.3): file, object, contents, "file per part", format, precision
// (Coarse / Normal / Fine / Custom), "Custom" fields with validation, mesh check; for the flat pattern DXF (S5-DXF-001)
// — sheet metal parts, bend lines and the contour check. ✓ is enabled only if parameters are valid and something is
// selected. Running the export belongs to the main window (file dialog).
#pragma once

#include "api/session.h"
#include "app/expr_field.h"
#include "app/check_list.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QAbstractButton>
#include <QWidget>

#include <optional>
#include <utility>

namespace construe::app {

class ExportPanel : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Stl, ThreeMf, FlatDxf };
    explicit ExportPanel(api::Session& session, QWidget* parent = nullptr);
    // Defaults on every open: "Binary", "Normal", "Custom" fields — the "Normal" values; bend lines on.
    void reset(Kind kind = Kind::Stl);
    // Chord (mm) and angle (rad) of the selected precision; a "Custom" field error — text in err, field in bad.
    std::optional<std::pair<double, double>> params(QString* err = nullptr, QLineEdit** bad = nullptr) const;
    // Export parameters (explicit tolerances when precision differs from "Normal"); nothing — a field has an error.
    std::optional<io::StlOptions> options() const;
    // Mesh export parameters (core-api §4.7а); nothing — a field has an error or nothing is selected.
    std::optional<api::Session::MeshExportOptions> meshOptions() const;
    bool threeMf() const { return file3mf_->isChecked(); }
    // Printer clearance from the settings (S6-PRINT-001, app-ux §10.3): caption of «For printing with tolerances», set
    // before reset().
    void setPrinterClearance(double mm);
    // Flat pattern DXF (S5-DXF-001): selected; export parameters (core-api §4.7е), nothing — nothing selected or the
    // contours do not pass the check.
    bool flat() const { return fileFlat_->isChecked(); }
    std::optional<api::Session::FlatExportOptions> flatOptions() const;

signals:
    void accepted();
    void cancelled();

private:
    void update();
    void fillItems();
    std::vector<CheckList::Item> contentRows() const;  // rows of «Contents» for the chosen object
    void runCheck();

    api::Session& session_;
    QAbstractButton* fileStl_ = nullptr;  // options of the file row (ChoiceRow)
    QAbstractButton* file3mf_ = nullptr;
    QAbstractButton* fileFlat_ = nullptr;
    QCheckBox* bendLines_ = nullptr;
    QCheckBox* print_ = nullptr;  // «For printing with tolerances» (S6-PRINT-001, -002)
    double clearance_ = printprep::PRINT_CLEARANCE_DEFAULT;
    QFormLayout* form_ = nullptr;
    QWidget* formatRow_ = nullptr;
    bool flatOk_ = false;  // the flat pattern check passed (flatParts without an error)
    QComboBox* object_ = nullptr;
    CheckList* items_ = nullptr;
    QCheckBox* perPart_ = nullptr;
    QLabel* check_ = nullptr;
    QLabel* title_ = nullptr;
    QAbstractButton* ascii_ = nullptr;  // options of the STL format row
    QAbstractButton* binary_ = nullptr;
    QComboBox* precision_ = nullptr;
    ExprField* chord_ = nullptr;
    ExprField* angle_ = nullptr;
    QLabel* chordLabel_ = nullptr;
    QPushButton* ok_ = nullptr;
};

}  // namespace construe::app
