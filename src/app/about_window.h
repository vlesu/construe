// «About» window (spec/app-ux.md §2.1а, S7-LIC-002, S7-LIC-003): modeless; version and OpenGL,
// the license identifier, the line about the user's files, the LICENSE text and the third-party component list —
// both from the program resources (the repository's LICENSE and THIRD_PARTY_NOTICES, docs/legal/README.md).
#pragma once

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QTabWidget>
#include <QTableWidget>
#include <QWidget>

#include <vector>

namespace construe::app {

// One component of THIRD_PARTY_NOTICES (docs/legal/README.md «Check»).
struct NoticeComponent {
    QString name, version, license, copyright, source, licenseText;
};
// Components of a THIRD_PARTY_NOTICES text in file order; blocks without "Component:" are skipped.
std::vector<NoticeComponent> parseNotices(const QString& text);
// The LICENSE and THIRD_PARTY_NOTICES texts embedded into the program; empty — the resource is missing.
QString licenseText();
QString noticesText();
// The SPDX license expression of the program, read from LICENSE at build time.
QString licenseSpdx();

class AboutWindow : public QWidget {
    Q_OBJECT
public:
    explicit AboutWindow(QWidget* parent);
    // Shows the window (raises an open one) with the header for the current OpenGL description.
    void open(const QString& glText);

private:
    QLabel* header_ = nullptr;
    QLabel* license_ = nullptr;
    QLabel* userFiles_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QPlainTextEdit* licenseText_ = nullptr;
    QTableWidget* components_ = nullptr;
    QPushButton* close_ = nullptr;
};

}  // namespace construe::app
