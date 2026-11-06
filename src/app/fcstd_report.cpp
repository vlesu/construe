// FreeCAD exchange report window (spec/app-ux.md §10.9, S4A-REP-001).
#include "app/fcstd_report.h"

#include "app/messages.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

namespace construe::app {

namespace {

const fcstd::ReportKind KINDS[] = {fcstd::ReportKind::Transferred, fcstd::ReportKind::Approximate, fcstd::ReportKind::BaseBody,
                                   fcstd::ReportKind::Skipped};

const char* groupKey(fcstd::ReportKind k) {
    switch (k) {
        case fcstd::ReportKind::Transferred: return "ui.fcstdReport.transferred";
        case fcstd::ReportKind::Approximate: return "ui.fcstdReport.approximate";
        case fcstd::ReportKind::BaseBody: return "ui.fcstdReport.baseBody";
        case fcstd::ReportKind::Skipped: return "ui.fcstdReport.skipped";
        default: return "ui.fcstdReport.skipped";
    }
}

QString reasons(const fcstd::ReportEntry& e) {
    QStringList out;
    for (const auto& r : e.reasons) out << fcstdReasonText(r);
    return out.join("; ");
}

QString line(const fcstd::ReportEntry& e) {
    QString s = QString::fromStdString(e.label.empty() ? e.object : e.label) + " (" + QString::fromStdString(e.type) + ")";
    if (!e.reasons.empty()) s += " — " + reasons(e);
    return s;
}

}  // namespace

QString fcstdReasonText(const std::string& reason) {
    const size_t c = reason.find(':');
    const std::string code = c == std::string::npos ? reason : reason.substr(0, c);
    const std::string detail = c == std::string::npos ? std::string() : reason.substr(c + 1);
    const std::string key = "ui.fcstdReport.reason." + code;
    const QString t = text(key, {{"detail", QString::fromStdString(detail)}});
    return t == QString::fromStdString(key) ? QString::fromStdString(reason) : t;
}

QString fcstdGroupTitle(fcstd::ReportKind kind, int count) { return text(groupKey(kind), {{"count", QString::number(count)}}); }

QString fcstdReportText(const QString& title, const std::vector<fcstd::ReportEntry>& entries) {
    QString out = title + "\n";
    for (auto k : KINDS) {
        int n = 0;
        for (const auto& e : entries) n += e.kind == k;
        if (n == 0) continue;
        out += "\n" + fcstdGroupTitle(k, n) + "\n";
        for (const auto& e : entries)
            if (e.kind == k) out += "  " + line(e) + "\n";
    }
    return out;
}

FcstdReportWindow::FcstdReportWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName("fcstdReport");
    resize(720, 520);
    auto* col = new QVBoxLayout(this);
    header_ = new QLabel;
    header_->setObjectName("fcstdReportHeader");
    header_->setWordWrap(true);
    col->addWidget(header_);
    tree_ = new QTreeWidget;
    tree_->setObjectName("fcstdReportTree");
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({text("ui.fcstdReport.object"), text("ui.fcstdReport.reasons")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    col->addWidget(tree_, 1);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    copy_ = new QPushButton(text("ui.fcstdReport.copy"));
    copy_->setObjectName("fcstdReportCopy");
    close_ = new QPushButton(text("ui.fcstdReport.close"));
    close_->setObjectName("fcstdReportClose");
    row->addWidget(copy_);
    row->addWidget(close_);
    col->addLayout(row);
    connect(copy_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_); });
    connect(close_, &QPushButton::clicked, this, &QWidget::close);
}

void FcstdReportWindow::open(const QString& title, const std::vector<fcstd::ReportEntry>& entries) {
    setWindowTitle(title);
    header_->setText(title);
    text_ = fcstdReportText(title, entries);
    tree_->clear();
    for (auto k : KINDS) {
        int n = 0;
        for (const auto& e : entries) n += e.kind == k;
        if (n == 0) continue;
        auto* g = new QTreeWidgetItem(tree_, {fcstdGroupTitle(k, n)});
        g->setData(0, Qt::UserRole, QString(fcstd::reportKindName(k)));
        for (const auto& e : entries) {
            if (e.kind != k) continue;
            auto* it = new QTreeWidgetItem(g, {QString::fromStdString(e.label.empty() ? e.object : e.label) + " (" +
                                                   QString::fromStdString(e.type) + ")",
                                               reasons(e)});
            it->setData(0, Qt::UserRole, QString::fromStdString(e.object));
            it->setToolTip(1, reasons(e));
        }
        g->setExpanded(k != fcstd::ReportKind::Transferred);
    }
    show();
    raise();
    activateWindow();
}

}  // namespace construe::app
