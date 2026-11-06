// Application settings (app/app_settings.h, spec/app-ux.md §10.6).
#include "app/app_settings.h"

#include "app/display_style.h"
#include "api/session.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

namespace construe::app {

const std::vector<AppSettings::Def>& AppSettings::definitions() {
    static const std::vector<Def> defs = [] {
        QStringList styles;
        for (DisplayStyle s : ALL_DISPLAY_STYLES) styles << styleName(s);
        return std::vector<Def>{
            {"view/mouseProfile", Type::String, QString("middleButton"), {"middleButton", "rightButton"}},
            {"view/invertWheel", Type::Bool, false, {}},
            {"view/displayStyle", Type::String, QString("shadedEdges"), styles},
            {"fonts/textFont", Type::String, QString(), {}},
            {"fonts/directories", Type::StringList, QStringList(), {}},
            {"drawing/fontGost", Type::String, QString(), {}},
            {"drawing/fontIso", Type::String, QString(), {}},
            {"drawing/standard", Type::String, QString("gost"), {"gost", "iso"}},
            {"drawing/projection", Type::String, QString("first"), {"first", "third"}},
            // Import (S4-REC-013): STEP shape healing tolerance and allowed dimension rounding of a recovered part, mm.
            {"import/healTolerance", Type::String, QString(HEAL_TOLERANCE_DEFAULT), HEAL_TOLERANCES},
            {"import/roundTolerance", Type::String, QString(ROUND_TOLERANCE_DEFAULT), ROUND_TOLERANCES},
            {"print/clearance", Type::String, QString(PRINT_CLEARANCE_DEFAULT), PRINT_CLEARANCES},  // printer clearance, mm (S6-PRINT-001)
            {"report/email", Type::String, QString(), {}},  // problem report e-mail (§10.8, S4-BUG-001)
            {"update/lastCheck", Type::String, QString(), {}},  // last update check start (§10.10, REL-CT-002)
            // Library of standard parts (S6-LIB-002, library.md §4): the user folder and shared folders.
            {"library/userDirectory", Type::String, QString(), {}},
            {"library/directories", Type::StringList, QStringList(), {}},
            // Standards shown in the library catalog (FX11-STD-001, library.md §2.7): both on by default, never both off.
            {"library/standardGost", Type::Bool, true, {}},
            {"library/standardIso", Type::Bool, true, {}},
            // Where new connections cut (S6-SOL-010, solutions.md §7.10): in the assembly or in the parts.
            {"solutions/cut", Type::String, QString("parts"), {"assembly", "parts"}},  // FX12-SOL-001: «В деталях» by default
            // Interface language (S8-I18N-001, §9.5): as in the system, Russian, English.
            {"ui/language", Type::String, QString("system"), {"system", "ru", "en"}},
        };
    }();
    // Title block values of new drawings (FX14-SET-001): any text, empty — the field is not filled.
    static const std::vector<Def> all = [] {
        std::vector<Def> v = defs;
        for (const QString& k : TITLE_BLOCK_DEFAULT_KEYS) v.push_back({"titleBlock/" + k, Type::String, QString(), {}});
        return v;
    }();
    return all;
}

const AppSettings::Def* AppSettings::find(const QString& key) {
    for (const auto& d : definitions())
        if (d.key == key) return &d;
    return nullptr;
}

std::optional<QVariant> AppSettings::validate(const Def& d, const QVariant& v) {
    switch (d.type) {
    case Type::Bool:
        if (v.typeId() != QMetaType::Bool) return std::nullopt;
        return v;
    case Type::String: {
        if (v.typeId() != QMetaType::QString) return std::nullopt;
        const QString s = v.toString();
        if (!d.allowed.isEmpty() && !d.allowed.contains(s)) return std::nullopt;
        return QVariant(s);
    }
    case Type::StringList: {
        if (v.typeId() == QMetaType::QStringList) return v;
        if (v.typeId() != QMetaType::QVariantList) return std::nullopt;
        QStringList out;
        for (const QVariant& x : v.toList()) {
            if (x.typeId() != QMetaType::QString) return std::nullopt;
            out << x.toString();
        }
        return QVariant(out);
    }
    }
    return std::nullopt;
}

namespace {
// Values written by earlier versions (§10.6 «Прежние значения», S7-TM-002): key, legacy value, current value.
struct LegacyValue {
    const char* key;
    const char* legacy;
    const char* current;
};
const LegacyValue LEGACY_VALUES[] = {
    {"view/mouseProfile", "solidworks", "middleButton"},  // legacy value
    {"view/mouseProfile", "onshape", "rightButton"},      // legacy value
};
}  // namespace

QVariant AppSettings::migrateLegacy(const Def& d, const QVariant& v) {
    if (v.typeId() != QMetaType::QString) return v;
    for (const auto& l : LEGACY_VALUES)
        if (d.key == QLatin1String(l.key) && v.toString() == QLatin1String(l.legacy)) return QString(l.current);
    return v;
}

namespace {
// Value from QSettings (the registry stores booleans as strings, a one-element list as a string) → validate form.
QVariant fromStore(const AppSettings::Def& d, const QVariant& v) {
    if (d.type == AppSettings::Type::Bool && v.typeId() == QMetaType::QString) {
        if (v.toString() == "true") return true;
        if (v.toString() == "false") return false;
    }
    if (d.type == AppSettings::Type::StringList && v.typeId() == QMetaType::QString) return QStringList{v.toString()};
    return v;
}
}  // namespace

AppSettings::AppSettings(std::unique_ptr<QSettings> store, QObject* parent) : QObject(parent), store_(std::move(store)) {
    for (const auto& d : definitions()) {
        values_[d.key] = d.defaultValue;
        if (!store_ || !store_->contains(d.key)) continue;
        const QVariant stored = fromStore(d, store_->value(d.key));
        const QVariant current = migrateLegacy(d, stored);
        if (auto v = validate(d, current)) {
            values_[d.key] = *v;
            if (current != stored) store_->setValue(d.key, *v);  // the store keeps only current values
        }
    }
    // Both library standards off is not a valid state (FX11-STD-001): both read as on.
    if (!values_["library/standardGost"].toBool() && !values_["library/standardIso"].toBool()) {
        values_["library/standardGost"] = true;
        values_["library/standardIso"] = true;
        if (store_) {
            store_->setValue("library/standardGost", true);
            store_->setValue("library/standardIso", true);
        }
    }
}

bool AppSettings::lastStandard(const QString& key, const QVariant& v) const {
    const QString other = key == "library/standardGost" ? "library/standardIso" : key == "library/standardIso" ? "library/standardGost" : QString();
    return !other.isEmpty() && !v.toBool() && !value(other).toBool();
}

QVariant AppSettings::value(const QString& key) const {
    auto it = values_.find(key);
    return it != values_.end() ? it->second : QVariant();
}

bool AppSettings::set(const QString& key, const QVariant& v0) {
    const Def* d = find(key);
    if (!d) return false;
    const auto v = validate(*d, v0);
    if (!v || lastStandard(key, *v)) return false;
    if (store_) store_->setValue(key, *v);
    if (values_[key] == *v) return true;
    values_[key] = *v;
    emit changed(key);
    return true;
}

void AppSettings::resetToDefaults() {
    for (const auto& d : definitions()) set(d.key, d.defaultValue);
}

std::map<std::string, std::string> AppSettings::titleBlockDefaults() const {
    std::map<std::string, std::string> out;
    for (const QString& k : TITLE_BLOCK_DEFAULT_KEYS) {
        const QString v = value("titleBlock/" + k).toString().trimmed();
        if (!v.isEmpty()) out[k.toStdString()] = v.toStdString();
    }
    return out;
}

std::string AppSettings::drawingFont(font::DrawingStandard s) const {
    const QString v = value(s == font::DrawingStandard::Iso ? "drawing/fontIso" : "drawing/fontGost").toString();
    if (!v.isEmpty()) {
        const auto names = api::Session::fonts();
        for (const auto& n : names)
            if (QString::fromStdString(n).compare(v, Qt::CaseInsensitive) == 0) return n;
    }
    return api::Session::defaultDrawingFont(s);
}

namespace {
// A single JSON value as text (via a one-element array: QJsonDocument writes only objects and arrays).
QByteArray jsonText(const QJsonValue& v) {
    const QByteArray s = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return s.mid(1, s.size() - 2);
}
}  // namespace

// Keys in table order (QJsonObject would sort them), so the text is assembled manually.
QByteArray AppSettings::exportJson() const {
    QByteArray body = "{\n  \"construe-settings\": 1,\n  \"settings\": {\n";
    const auto& defs = definitions();
    for (size_t i = 0; i < defs.size(); ++i) {
        const auto& d = defs[i];
        QJsonValue jv;
        if (d.type == Type::Bool) jv = value(d.key).toBool();
        else if (d.type == Type::String) jv = value(d.key).toString();
        else jv = QJsonArray::fromStringList(value(d.key).toStringList());
        body += "    " + jsonText(d.key) + ": " + jsonText(jv) + (i + 1 < defs.size() ? ",\n" : "\n");
    }
    body += "  }\n}\n";
    return body;
}

AppSettings::ImportResult AppSettings::importJson(const QByteArray& data) {
    ImportResult r;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return r;
    const QJsonObject root = doc.object();
    if (!root.value("construe-settings").isDouble() || root.value("construe-settings").toDouble() != 1 || !root.value("settings").isObject())
        return r;
    r.isSettings = true;
    // File order (§10.6): QJsonObject sorts keys — the order is restored from each key's position in the text.
    const QJsonObject settings = root.value("settings").toObject();
    QStringList keys = settings.keys();
    std::stable_sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) {
        return data.indexOf(jsonText(a)) < data.indexOf(jsonText(b));
    });
    for (const QString& k : keys) {
        const Def* d = find(k);
        if (!d) {
            r.skipped.push_back({k, true});
            continue;
        }
        const QVariant v = migrateLegacy(*d, settings.value(k).toVariant());
        if (!validate(*d, v) || !set(k, v)) {  // set rejects switching off the last library standard
            r.skipped.push_back({k, false});
            continue;
        }
        ++r.applied;
    }
    return r;
}

std::vector<std::string> fontLibraryDirectories(const QString& dataDir, const QStringList& userDirs, const QStringList& systemDirs) {
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    QStringList system;
    for (const QString& d : systemDirs) system << QDir::cleanPath(QDir::fromNativeSeparators(d));
    std::vector<std::string> dirs;
    if (!dataDir.isEmpty()) {
        const QString own = QDir::cleanPath(dataDir + "/fonts");
        QDir().mkpath(own);
        dirs.push_back(own.toStdString());
    }
    for (const QString& d : userDirs) {
        const QString c = QDir::cleanPath(QDir::fromNativeSeparators(d));
        if (!c.isEmpty() && !system.contains(c, cs)) dirs.push_back(c.toStdString());
    }
    for (auto& d : api::Session::defaultFontDirectories()) dirs.push_back(d);
    return dirs;
}

QStringList systemFontDirectories() { return QStandardPaths::standardLocations(QStandardPaths::FontsLocation); }

}  // namespace construe::app
