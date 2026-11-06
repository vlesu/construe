// FreeCAD exchange report (spec/app-ux.md §10.9, S4A-REP-001): the window with the objects of an import or an export
// grouped as transferred, approximate, base body and skipped, with reasons in words; the report can be copied.
#pragma once

#include "fcstd/import.h"

#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTreeWidget>
#include <QWidget>

#include <vector>

namespace construe::app {

// Text of a reason code "code[:detail]" (spec/fcstd.md §9, §12е.3); an unknown code — the code itself.
QString fcstdReasonText(const std::string& reason);
// Group title with the count: «Transferred (3)» etc.
QString fcstdGroupTitle(fcstd::ReportKind kind, int count);
// Plain text of the report for the clipboard (§10.9): title line, then the groups in order with one line per object
// «<label> (<type>) — <reasons>».
QString fcstdReportText(const QString& title, const std::vector<fcstd::ReportEntry>& entries);

class FcstdReportWindow : public QWidget {
    Q_OBJECT
public:
    explicit FcstdReportWindow(QWidget* parent);
    // Shows the window (raises an open one) with the report; title — ui.fcstdReport.import/export with the file name.
    void open(const QString& title, const std::vector<fcstd::ReportEntry>& entries);
    QString reportText() const { return text_; }

private:
    QLabel* header_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* close_ = nullptr;
    QString text_;
};

}  // namespace construe::app
