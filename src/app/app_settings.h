// Application settings (spec/app-ux.md §10.6, S3-SET-001, S3-SET-002): keys, value validation, storage in QSettings,
// JSON export and import. No widgets — the main window applies the settings on the changed signal.
#pragma once

#include "font/font.h"  // DrawingStandard type (published by api::Session)

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace construe::app {

// Import settings values (app-ux §10.6; S4-REC-013), mm; "0" — off.
inline const QStringList HEAL_TOLERANCES = {"0", "0.01", "0.05", "0.1", "0.5"};
inline constexpr const char* HEAL_TOLERANCE_DEFAULT = "0.05";
inline const QStringList ROUND_TOLERANCES = {"0", "0.0001", "0.001", "0.01", "0.1"};
inline constexpr const char* ROUND_TOLERANCE_DEFAULT = "0.001";
// Printer clearance values (app-ux §10.6, printprep.md §4.1; S6-PRINT-001), mm.
inline const QStringList PRINT_CLEARANCES = {"0", "0.05", "0.1", "0.15", "0.2", "0.25", "0.3", "0.4", "0.5"};
inline constexpr const char* PRINT_CLEARANCE_DEFAULT = "0.2";

// Title block fields with a default value for new drawings (app-ux §10.6, FX14-SET-001): keys of drawing.md §5.4 and
// §5а.4; the setting key — "titleBlock/" + key.
inline const QStringList TITLE_BLOCK_DEFAULT_KEYS = {
    "organization", "developer",  "developerDate",   "checker",  "checkerDate", "techControl", "techControlDate", "extraRole", "extraName",
    "extraDate",    "normControl", "normControlDate", "approver", "approverDate", "department", "issueDate",       "language"};

class AppSettings : public QObject {
    Q_OBJECT
public:
    enum class Type { String, Bool, StringList };
    struct Def {
        QString key;
        Type type;
        QVariant defaultValue;
        QStringList allowed;  // for strings: allowed values; empty — any string
    };
    // Key table of §10.6 in its order.
    static const std::vector<Def>& definitions();
    static const Def* find(const QString& key);
    // Validation of a JSON-typed value (QVariant from QJsonValue): the coerced value or nullopt — invalid.
    static std::optional<QVariant> validate(const Def& d, const QVariant& v);
    // A value written by an earlier version → its current equivalent (§10.6, S7-TM-002); any other value unchanged.
    // Applied when reading QSettings and on import; set() takes current values only.
    static QVariant migrateLegacy(const Def& d, const QVariant& v);

    // store — persistent storage; nullptr — in memory only (test mode, §12.1).
    explicit AppSettings(std::unique_ptr<QSettings> store, QObject* parent = nullptr);

    QVariant value(const QString& key) const;
    // Unknown key or invalid value — false, no changes; also false when the value would switch off both library
    // standards (FX11-STD-001). Otherwise saves; changed — if the value differs.
    bool set(const QString& key, const QVariant& v);
    // The value v of key would leave both library standards off (FX11-STD-001).
    bool lastStandard(const QString& key, const QVariant& v) const;
    void resetToDefaults();  // all keys to defaults (test mode)

    // Typed access.
    QString mouseProfile() const { return value("view/mouseProfile").toString(); }
    bool invertWheel() const { return value("view/invertWheel").toBool(); }
    QString displayStyle() const { return value("view/displayStyle").toString(); }
    QString textFont() const { return value("fonts/textFont").toString(); }
    QStringList fontDirectories() const { return value("fonts/directories").toStringList(); }
    double healTolerance() const { return value("import/healTolerance").toString().toDouble(); }    // mm, 0 — no healing
    double roundTolerance() const { return value("import/roundTolerance").toString().toDouble(); }  // mm, 0 — exact values
    double printClearance() const { return value("print/clearance").toString().toDouble(); }        // mm (S6-PRINT-001)
    bool libraryGost() const { return value("library/standardGost").toBool(); }  // FX11-STD-001
    bool libraryIso() const { return value("library/standardIso").toBool(); }
    std::string connectionCut() const { return value("solutions/cut").toString().toStdString(); }   // S6-SOL-010
    // Title block values of a new drawing (FX14-SET-001): the non-empty «titleBlock/…» settings, key → value.
    std::map<std::string, std::string> titleBlockDefaults() const;
    // Drawing font per standard (§10.6): the configured one if present in the library, else font::defaultDrawingFont.
    std::string drawingFont(font::DrawingStandard s) const;

    // JSON export and import (§10.6).
    QByteArray exportJson() const;
    struct ImportResult {
        bool isSettings = false;  // the file is a JSON object with "construe-settings": 1
        int applied = 0;
        struct Skip {
            QString key;
            bool unknown = false;  // unknown key; otherwise an invalid value
        };
        std::vector<Skip> skipped;  // in file order
    };
    ImportResult importJson(const QByteArray& data);

signals:
    void changed(const QString& key);

private:
    std::unique_ptr<QSettings> store_;
    std::map<QString, QVariant> values_;
};

// Font library directories (font.md §2, FX10-DRW-004): the user's fonts folder <dataDir>/fonts (created when missing;
// none with an empty dataDir), then the folders of the setting fonts/directories except the system font folders
// (systemDirs; compared after cleaning, case-insensitive on Windows), then the shipped ones (defaultFontDirectories).
std::vector<std::string> fontLibraryDirectories(const QString& dataDir, const QStringList& userDirs, const QStringList& systemDirs);
// System font folders of the platform (QStandardPaths::FontsLocation): never offered in the font lists.
QStringList systemFontDirectories();

}  // namespace construe::app
