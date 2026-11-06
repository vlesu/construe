#include "app/about_window.h"

#include "app/messages.h"
#include "app/update_check.h"

#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

namespace construe::app {

namespace {

QString readResource(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).replace("\r\n", "\n") : QString();
}

}  // namespace

std::vector<NoticeComponent> parseNotices(const QString& text) {
    std::vector<NoticeComponent> out;
    NoticeComponent cur;
    bool have = false;
    const auto flush = [&] {
        if (have && !cur.name.isEmpty()) out.push_back(cur);
        cur = {};
        have = false;
    };
    for (QString line : QString(text).replace("\r\n", "\n").split('\n')) {
        if (line.startsWith("----")) {  // the block separator (80 '-')
            flush();
            continue;
        }
        const int colon = line.indexOf(": ");
        if (colon < 0) continue;
        const QString key = line.left(colon), value = line.mid(colon + 2).trimmed();
        QString* field = key == "Component" ? &cur.name
                         : key == "Version" ? &cur.version
                         : key == "License" ? &cur.license
                         : key == "Copyright" ? &cur.copyright
                         : key == "Source" ? &cur.source
                         : key == "License text" ? &cur.licenseText
                                                 : nullptr;
        if (!field) continue;
        *field = value;
        have = true;
    }
    flush();
    return out;
}

QString licenseText() { return readResource(":/legal/LICENSE"); }
QString noticesText() { return readResource(":/legal/THIRD_PARTY_NOTICES"); }
QString licenseSpdx() { return QStringLiteral(CONSTRUE_LICENSE_SPDX); }

AboutWindow::AboutWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName("aboutWindow");
    setWindowTitle(text("ui.about.title"));
    resize(760, 560);
    auto* col = new QVBoxLayout(this);
    header_ = new QLabel;
    header_->setObjectName("aboutHeader");
    header_->setWordWrap(true);
    header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    license_ = new QLabel(text("ui.about.license", {{"spdx", licenseSpdx()}}));
    license_->setObjectName("aboutLicense");
    license_->setWordWrap(true);
    license_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    userFiles_ = new QLabel(text("ui.about.userFiles"));
    userFiles_->setObjectName("aboutUserFiles");
    userFiles_->setWordWrap(true);
    for (QLabel* l : {header_, license_, userFiles_}) col->addWidget(l);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("aboutTabs");
    licenseText_ = new QPlainTextEdit(licenseText());
    licenseText_->setObjectName("aboutLicenseText");
    licenseText_->setReadOnly(true);
    licenseText_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("monospace");
    mono.setStyleHint(QFont::TypeWriter);
    licenseText_->setFont(mono);
    tabs_->addTab(licenseText_, text("ui.about.tabLicense"));
    const auto comps = parseNotices(noticesText());
    components_ = new QTableWidget(int(comps.size()), 3);
    components_->setObjectName("aboutComponents");
    components_->setHorizontalHeaderLabels({text("ui.about.colComponent"), text("ui.about.colVersion"), text("ui.about.colLicense")});
    components_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    components_->verticalHeader()->hide();
    components_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int r = 0; r < int(comps.size()); ++r) {
        const auto& c = comps[size_t(r)];
        const QString tip = c.copyright + "\n" + c.source + "\n" + c.licenseText;
        int k = 0;
        for (const QString& v : {c.name, c.version, c.license}) {
            auto* item = new QTableWidgetItem(v);
            item->setToolTip(tip);
            components_->setItem(r, k++, item);
        }
    }
    components_->resizeColumnsToContents();
    tabs_->addTab(components_, text("ui.about.tabComponents"));
    col->addWidget(tabs_, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    close_ = new QPushButton(text("ui.about.close"));
    close_->setObjectName("aboutClose");
    buttons->addWidget(close_);
    col->addLayout(buttons);
    connect(close_, &QPushButton::clicked, this, &QWidget::close);
}

void AboutWindow::open(const QString& glText) {
    header_->setText(text("ui.about.text", {{"version", versionText(currentVersion())}, {"gl", glText}}));
    show();
    raise();
    activateWindow();
}

}  // namespace construe::app
