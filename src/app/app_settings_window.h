// «Program settings» window (spec/app-ux.md §10.6, S3-SET-001, S3-SET-002): modeless,
// tabs, changes apply immediately; JSON export and import.
#pragma once

#include "app/app_settings.h"
#include "app/path_list_edit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QWidget>

#include <map>

namespace construe::app {

class AppSettingsWindow : public QWidget {
    Q_OBJECT
public:
    enum Tab { TabMouse = 0, TabDisplay = 1, TabFonts = 2, TabDrawing = 3, TabImport = 4, TabLibrary = 5, TabPrint = 6, TabLanguage = 7, TabTitleBlock = 8 };
    AppSettingsWindow(AppSettings& settings, bool testMode, QWidget* parent);
    void showTab(int tab);  // show the window (on top) on a tab; tab < 0 — the current one
    void sync();            // controls ← settings
    QString message() const { return message_->text(); }

private:
    void fillFontCombo(QComboBox* c, const QString& autoFont, const QString& current);
    void exportSettings();
    void importSettings();
    void chooseUserLibraryDir();

    AppSettings& settings_;
    bool testMode_;
    bool syncing_ = false;
    QTabWidget* tabs_ = nullptr;
    QComboBox* mouse_ = nullptr;
    QCheckBox* invert_ = nullptr;
    QComboBox* style_ = nullptr;
    QComboBox* textFont_ = nullptr;
    PathListEdit* dirs_ = nullptr;  // custom font folders
    QComboBox* gost_ = nullptr;
    QComboBox* iso_ = nullptr;
    QComboBox* standard_ = nullptr;    // standard for new drawings (S3-DRW-008)
    QComboBox* projection_ = nullptr;  // projection method for new ISO drawings
    QComboBox* heal_ = nullptr;        // STEP shape healing tolerance (S4-REC-013)
    QComboBox* round_ = nullptr;       // allowed dimension rounding for a recovered part
    QLineEdit* libUser_ = nullptr;     // user library folder (S6-LIB-002)
    PathListEdit* libDirs_ = nullptr;  // shared library folders
    QCheckBox* libGost_ = nullptr;     // library standards shown (FX11-STD-001)
    QCheckBox* libIso_ = nullptr;
    QComboBox* printClearance_ = nullptr;  // printer clearance (S6-PRINT-001)
    QComboBox* connectionCut_ = nullptr;   // where new connections cut (S6-SOL-010)
    QComboBox* language_ = nullptr;        // interface language (S8-I18N-001)
    std::map<QString, QLineEdit*> titleBlock_;  // «titleBlock/…» setting → its field (FX14-SET-001)
    QLabel* message_ = nullptr;
};

}  // namespace construe::app
