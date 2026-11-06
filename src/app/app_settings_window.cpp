// «Program settings» window (app/app_settings_window.h, spec/app-ux.md §10.6).
#include "app/app_settings_window.h"

#include "app/display_style.h"
#include "app/messages.h"
#include "api/session.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QSaveFile>
#include <QScrollArea>
#include <QVBoxLayout>

namespace construe::app {

namespace {

const std::pair<DisplayStyle, const char*> STYLE_LABELS[] = {{DisplayStyle::ShadedEdges, "ui.common.shadedWithEdges"},
                                                             {DisplayStyle::Shaded, "ui.common.shaded"},
                                                             {DisplayStyle::HiddenRemoved, "ui.common.hiddenLinesRemoved"},
                                                             {DisplayStyle::HiddenDashed, "ui.common.hiddenLinesDashed"},
                                                             {DisplayStyle::Wireframe, "ui.common.wireframe"}};

}  // namespace

AppSettingsWindow::AppSettingsWindow(AppSettings& settings, bool testMode, QWidget* parent)
    : QWidget(parent, Qt::Window), settings_(settings), testMode_(testMode) {
    setObjectName("appSettingsWindow");
    setWindowTitle(txt("ui.settings.programSettings"));
    auto* v = new QVBoxLayout(this);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("appSettingsTabs");
    v->addWidget(tabs_, 1);

    // Mouse and wheel.
    auto* mouseTab = new QWidget;
    auto* mf = new QFormLayout(mouseTab);
    mouse_ = new QComboBox;
    mouse_->setObjectName("setMouseProfile");
    mouse_->addItem(txt("ui.settings.rotateWithMiddleButton"), "middleButton");
    mouse_->addItem(txt("ui.settings.rotateWithRightButtonFor"), "rightButton");
    mf->addRow(txt("ui.settings.mouseProfile"), mouse_);
    invert_ = new QCheckBox(txt("ui.settings.invertWheelZoom"));
    invert_->setObjectName("setInvertWheel");
    mf->addRow(invert_);
    tabs_->addTab(mouseTab, txt("ui.settings.mouseAndWheel"));

    // Display.
    auto* dispTab = new QWidget;
    auto* df = new QFormLayout(dispTab);
    style_ = new QComboBox;
    style_->setObjectName("setDisplayStyle");
    for (const auto& [st, label] : STYLE_LABELS) style_->addItem(QString::fromUtf8(txt(label)), QString(styleName(st)));
    df->addRow(txt("ui.settings.displayStyle"), style_);
    tabs_->addTab(dispTab, txt("ui.common.display"));

    // Fonts.
    auto* fontTab = new QWidget;
    auto* ff = new QFormLayout(fontTab);
    textFont_ = new QComboBox;
    textFont_->setObjectName("setTextFont");
    ff->addRow(txt("ui.settings.defaultTextFont"), textFont_);
    dirs_ = new PathListEdit({"setFontDirs", "setFontDirAdd", "setFontDirRemove"}, txt("ui.settings.addFolder"), txt("ui.settings.removeFolder"));
    dirs_->setFolderDialog("ui.settings.fontFolder", testMode_);
    ff->addRow(txt("ui.settings.customFontFolders"), dirs_);
    tabs_->addTab(fontTab, txt("ui.settings.fonts"));

    // Drawing style.
    auto* drawTab = new QWidget;
    auto* wf = new QFormLayout(drawTab);
    gost_ = new QComboBox;
    gost_->setObjectName("setDrawingFontGost");
    wf->addRow(txt("ui.settings.gostDrawingFont"), gost_);
    iso_ = new QComboBox;
    iso_->setObjectName("setDrawingFontIso");
    wf->addRow(txt("ui.settings.isoDrawingFont"), iso_);
    standard_ = new QComboBox;
    standard_->setObjectName("setDrawingStandard");
    standard_->addItem(txt("ui.common.gost"), QString("gost"));
    standard_->addItem("ISO", QString("iso"));
    wf->addRow(txt("ui.settings.standardForNewDrawings"), standard_);
    projection_ = new QComboBox;
    projection_->setObjectName("setDrawingProjection");
    projection_->addItem(txt("ui.common.firstAngle"), QString("first"));
    projection_->addItem(txt("ui.common.thirdAngle"), QString("third"));
    wf->addRow(txt("ui.settings.projectionMethodIso"), projection_);
    tabs_->addTab(drawTab, txt("ui.settings.drawingStyle"));

    // Import (S4-REC-013): items are values in mm, «0» means «Off».
    auto* importTab = new QWidget;
    auto* itf = new QFormLayout(importTab);
    auto fillTol = [](QComboBox* c, const QStringList& values) {
        for (const auto& v : values) c->addItem(v == "0" ? QString(txt("ui.common.off")) : v + txt("ui.common.mm"), v);
    };
    heal_ = new QComboBox;
    heal_->setObjectName("setHealTolerance");
    fillTol(heal_, HEAL_TOLERANCES);
    itf->addRow(txt("ui.settings.stepShapeHealingTolerance"), heal_);
    round_ = new QComboBox;
    round_->setObjectName("setRoundTolerance");
    fillTol(round_, ROUND_TOLERANCES);
    itf->addRow(txt("ui.settings.roundingOfReconstructedPartDimensions"), round_);
    tabs_->addTab(importTab, txt("ui.common.import"));

    // Library of standard parts (S6-LIB-002, library.md §4): the user folder (saving elements), shared folders.
    auto* libTab = new QWidget;
    auto* lf = new QFormLayout(libTab);
    auto* userRow = new QHBoxLayout;
    libUser_ = new QLineEdit;
    libUser_->setObjectName("setLibUserDir");
    libUser_->setReadOnly(true);
    auto* userChange = new QPushButton(txt("ui.settings.change"));
    userChange->setObjectName("setLibUserDirChange");
    userRow->addWidget(libUser_, 1);
    userRow->addWidget(userChange);
    lf->addRow(txt("ui.settings.userFolder"), userRow);
    libDirs_ = new PathListEdit({"setLibDirs", "setLibDirAdd", "setLibDirRemove"}, txt("ui.settings.addFolder"), txt("ui.settings.removeFolder"));
    libDirs_->setFolderDialog("ui.settings.libraryFolder", testMode_);
    lf->addRow(txt("ui.settings.sharedFolders"), libDirs_);
    // Standards shown in the catalog (FX11-STD-001): GOST and ISO; switching off the last one is rejected.
    auto* stdRow = new QHBoxLayout;
    libGost_ = new QCheckBox(txt("ui.settings.stdGost"));
    libGost_->setObjectName("setLibStdGost");
    libIso_ = new QCheckBox(txt("ui.settings.stdIso"));
    libIso_->setObjectName("setLibStdIso");
    stdRow->addWidget(libGost_);
    stdRow->addWidget(libIso_);
    stdRow->addStretch(1);
    lf->addRow(txt("ui.settings.libraryStandards"), stdRow);
    connectionCut_ = new QComboBox;  // S6-SOL-010, solutions.md §7.10
    connectionCut_->setObjectName("setConnectionCut");
    connectionCut_->addItem(text("ui.solution.cutAssembly"), "assembly");
    connectionCut_->addItem(text("ui.solution.cutParts"), "parts");
    lf->addRow(txt("ui.settings.whereToCutConnections"), connectionCut_);
    tabs_->addTab(libTab, txt("ui.settings.library"));
    // Print (S6-PRINT-001): printer clearance for export "for printing with tolerances", values in mm.
    auto* printTab = new QWidget;
    auto* ptf = new QFormLayout(printTab);
    printClearance_ = new QComboBox;
    printClearance_->setObjectName("setPrintClearance");
    for (const auto& v : PRINT_CLEARANCES) printClearance_->addItem(v + txt("ui.common.mm"), v);
    ptf->addRow(txt("ui.settings.printerClearance"), printClearance_);
    tabs_->addTab(printTab, txt("ui.common.print"));

    // Interface language (S8-I18N-001, §9.5): language names — each in its own language.
    auto* langTab = new QWidget;
    auto* lgf = new QFormLayout(langTab);
    language_ = new QComboBox;
    language_->setObjectName("setLanguage");
    language_->addItem(txt("ui.settings.languageSystem"), "system");
    language_->addItem(QString::fromUtf8("Русский"), "ru");  // i18n-skip: language autonym
    language_->addItem("English", "en");
    lgf->addRow(txt("ui.settings.language"), language_);
    tabs_->addTab(langTab, txt("ui.settings.languageTab"));

    // Title block values of new drawings (FX14-SET-001, §10.6): a field per key of TITLE_BLOCK_DEFAULT_KEYS.
    {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget;
        auto* tf = new QFormLayout(body);
        auto* hint = new QLabel(txt("ui.settings.titleBlockHint"));
        hint->setObjectName("setTitleBlockHint");
        hint->setWordWrap(true);
        tf->addRow(hint);
        const std::pair<const char*, const char*> rows[] = {
            {"organization", "ui.common.organization"}, {"developer", "ui.common.drawnBy"},        {"developerDate", "ui.settings.drawnByDate"},
            {"checker", "ui.common.checkedBy"},         {"checkerDate", "ui.settings.checkedByDate"}, {"techControl", "ui.drawing.techControl"},
            {"techControlDate", "ui.settings.techControlDate"}, {"extraRole", "ui.common.extraFieldRole"}, {"extraName", "ui.common.extraField"},
            {"extraDate", "ui.settings.extraFieldDate"}, {"normControl", "ui.common.stdControl"},  {"normControlDate", "ui.settings.stdControlDate"},
            {"approver", "ui.common.approvedBy"},       {"approverDate", "ui.settings.approvedByDate"}, {"department", "ui.drawing.department"},
            {"issueDate", "ui.drawing.issueDate"},      {"language", "ui.drawing.language"}};
        for (const auto& [key, label] : rows) {
            auto* e = new QLineEdit;
            e->setObjectName(QString("setTitleBlock_") + key);
            const QString settingKey = QString("titleBlock/") + key;
            connect(e, &QLineEdit::editingFinished, this, [this, e, settingKey] {
                if (!e->isModified()) return;
                e->setModified(false);
                settings_.set(settingKey, e->text().trimmed());
            });
            tf->addRow(txt(label), e);
            titleBlock_[settingKey] = e;
        }
        scroll->setWidget(body);
        tabs_->addTab(scroll, txt("ui.settings.titleBlockTab"));
    }

    // Export, import, close, message.
    auto* bottom = new QHBoxLayout;
    auto* exp = new QPushButton(txt("ui.settings.exportSettings"));
    exp->setObjectName("setExport");
    auto* imp = new QPushButton(txt("ui.settings.importSettings"));
    imp->setObjectName("setImport");
    auto* close = new QPushButton(txt("ui.common.close"));
    close->setObjectName("setClose");
    bottom->addWidget(exp);
    bottom->addWidget(imp);
    bottom->addStretch(1);
    bottom->addWidget(close);
    v->addLayout(bottom);
    message_ = new QLabel;
    message_->setObjectName("setMessage");
    message_->setWordWrap(true);
    v->addWidget(message_);

    auto bindCombo = [this](QComboBox* c, const char* key) {
        connect(c, &QComboBox::currentIndexChanged, this, [this, c, key] {
            if (!syncing_) settings_.set(key, c->currentData().toString());
        });
    };
    bindCombo(mouse_, "view/mouseProfile");
    bindCombo(style_, "view/displayStyle");
    bindCombo(textFont_, "fonts/textFont");
    bindCombo(gost_, "drawing/fontGost");
    bindCombo(iso_, "drawing/fontIso");
    bindCombo(standard_, "drawing/standard");
    bindCombo(projection_, "drawing/projection");
    bindCombo(heal_, "import/healTolerance");
    bindCombo(round_, "import/roundTolerance");
    bindCombo(printClearance_, "print/clearance");
    bindCombo(connectionCut_, "solutions/cut");
    connect(language_, &QComboBox::currentIndexChanged, this, [this] {
        if (syncing_) return;
        if (settings_.set("ui/language", language_->currentData().toString())) message_->setText(text("ui.settings.languageChanged"));
    });
    auto bindStandard = [this](QCheckBox* c, const char* key) {
        connect(c, &QCheckBox::toggled, this, [this, c, key](bool on) {
            if (syncing_) return;
            if (settings_.set(key, on)) return;
            message_->setText(text("ui.settings.lastStandard"));
            sync();  // the box is checked again
        });
    };
    bindStandard(libGost_, "library/standardGost");
    bindStandard(libIso_, "library/standardIso");
    connect(invert_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) settings_.set("view/invertWheel", on);
    });
    connect(dirs_, &PathListEdit::pathsEdited, this, [this](const QStringList& d) { settings_.set("fonts/directories", d); });
    connect(dirs_, &PathListEdit::removeRequested, this, [this](int row) {
        QStringList d = settings_.fontDirectories();
        if (row >= d.size()) return;
        d.removeAt(row);
        settings_.set("fonts/directories", d);
    });
    connect(userChange, &QPushButton::clicked, this, [this] { chooseUserLibraryDir(); });
    connect(libDirs_, &PathListEdit::pathsEdited, this, [this](const QStringList& d) { settings_.set("library/directories", d); });
    connect(libDirs_, &PathListEdit::removeRequested, this, [this](int row) {
        QStringList d = settings_.value("library/directories").toStringList();
        if (row >= d.size()) return;
        d.removeAt(row);
        settings_.set("library/directories", d);
    });
    connect(exp, &QPushButton::clicked, this, [this] { exportSettings(); });
    connect(imp, &QPushButton::clicked, this, [this] { importSettings(); });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(&settings_, &AppSettings::changed, this, [this] { sync(); });
    sync();
    resize(560, 360);
}

void AppSettingsWindow::showTab(int tab) {
    if (tab >= 0) tabs_->setCurrentIndex(tab);
    sync();
    show();
    raise();
    activateWindow();
}

void AppSettingsWindow::fillFontCombo(QComboBox* c, const QString& autoFont, const QString& current) {
    c->clear();
    c->addItem(text("ui.settings.auto", {{"font", autoFont}}), QString());
    for (const auto& n : api::Session::fonts()) c->addItem(QString::fromStdString(n), QString::fromStdString(n));
    int i = current.isEmpty() ? 0 : c->findData(current, Qt::UserRole, Qt::MatchFixedString);
    if (i < 0) {  // the saved font is not in the library — it is shown last, the value is not lost
        c->addItem(current, current);
        i = c->count() - 1;
    }
    c->setCurrentIndex(i);
}

void AppSettingsWindow::sync() {
    syncing_ = true;
    mouse_->setCurrentIndex(std::max(0, mouse_->findData(settings_.mouseProfile())));
    invert_->setChecked(settings_.invertWheel());
    style_->setCurrentIndex(std::max(0, style_->findData(settings_.displayStyle())));
    fillFontCombo(textFont_, QString::fromStdString(api::Session::builtinDefaultFont()), settings_.textFont());
    fillFontCombo(gost_, QString::fromStdString(api::Session::defaultDrawingFont(font::DrawingStandard::Gost)), settings_.value("drawing/fontGost").toString());
    fillFontCombo(iso_, QString::fromStdString(api::Session::defaultDrawingFont(font::DrawingStandard::Iso)), settings_.value("drawing/fontIso").toString());
    standard_->setCurrentIndex(std::max(0, standard_->findData(settings_.value("drawing/standard").toString())));
    projection_->setCurrentIndex(std::max(0, projection_->findData(settings_.value("drawing/projection").toString())));
    heal_->setCurrentIndex(std::max(0, heal_->findData(settings_.value("import/healTolerance").toString())));
    round_->setCurrentIndex(std::max(0, round_->findData(settings_.value("import/roundTolerance").toString())));
    printClearance_->setCurrentIndex(std::max(0, printClearance_->findData(settings_.value("print/clearance").toString())));
    connectionCut_->setCurrentIndex(std::max(0, connectionCut_->findData(settings_.value("solutions/cut").toString())));
    language_->setCurrentIndex(std::max(0, language_->findData(settings_.value("ui/language").toString())));
    dirs_->setPaths(settings_.fontDirectories());
    libUser_->setText(settings_.value("library/userDirectory").toString());
    libUser_->setPlaceholderText(txt("ui.settings.defaultProgramDataFolder"));
    libDirs_->setPaths(settings_.value("library/directories").toStringList());
    libGost_->setChecked(settings_.libraryGost());
    libIso_->setChecked(settings_.libraryIso());
    for (auto& [key, e] : titleBlock_) {
        const QString v = settings_.value(key).toString();
        if (!(e->hasFocus() && e->isModified()) && e->text() != v) e->setText(v);
    }
    syncing_ = false;
}

void AppSettingsWindow::chooseUserLibraryDir() {
    QFileDialog::Options opt = QFileDialog::ShowDirsOnly;
    if (testMode_) opt |= QFileDialog::DontUseNativeDialog;
    const QString dir = QFileDialog::getExistingDirectory(this, txt("ui.settings.userLibraryFolder"), {}, opt);
    if (dir.isEmpty()) return;
    settings_.set("library/userDirectory", QDir::toNativeSeparators(QDir::cleanPath(dir)));
}

void AppSettingsWindow::exportSettings() {
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    QString path = QFileDialog::getSaveFileName(this, txt("ui.settings.exportSettings2"), "construe-settings.json", txt("ui.settings.construeSettingsJson"), nullptr, opt);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".json";
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(settings_.exportJson()) < 0 || !f.commit()) {
        message_->setText(text("error.IO_WRITE", {{"file", QFileInfo(path).fileName()}, {"detail", f.errorString()}}));
        return;
    }
    message_->setText(text("ui.settings.exported", {{"file", QFileInfo(path).fileName()}}));
}

void AppSettingsWindow::importSettings() {
    QFileDialog::Options opt = testMode_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.settings.importSettings2"), {}, txt("ui.settings.construeSettingsJson"), nullptr, opt);
    if (path.isEmpty()) return;
    QFile f(path);
    const QByteArray data = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    const auto r = settings_.importJson(data);
    const QString file = QFileInfo(path).fileName();
    if (!r.isSettings) {
        message_->setText(text("ui.settings.notSettings", {{"file", file}}));
        return;
    }
    QStringList lines{text("ui.settings.imported", {{"file", file}})};
    for (const auto& sk : r.skipped) lines << text(sk.unknown ? "ui.settings.unknownKey" : "ui.settings.badValue", {{"key", sk.key}});
    message_->setText(lines.join("\n"));
}

}  // namespace construe::app
