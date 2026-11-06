// Running without installation (spec/app-ux.md §10.5а, S8-PKG-001): the marker file next to the program and where the
// program keeps its data, cache and settings — next to itself, in the profile when its folder is read-only, or in the
// usual places of an installed program.
#pragma once

#include <QSettings>
#include <QString>

#include <memory>

namespace construe::app {

// Constants of app-ux §10.5а.
inline constexpr const char* PORTABLE_MARKER_FILE = "construe-portable.txt";
inline constexpr const char* PORTABLE_DATA_DIR = "construe-data";
inline constexpr const char* PORTABLE_SETTINGS_FILE = "settings.ini";

struct StorageLayout {
    bool portable = false;  // the marker is next to the program
    bool beside = false;    // portable and the data folder next to the program is writable
    QString dataDir;        // recent files, recovery copies, logs, crash report, user library (§10.5)
    QString cacheDir;       // base of caches: the evaluator cache is <cacheDir>/regen
    QString settingsFile;   // INI file of the settings; empty — QSettings("construe", "construe") (the registry)
};

// The marker PORTABLE_MARKER_FILE is in appDir.
bool portableMarkerPresent(const QString& appDir);
// dir exists or is created, and a probe file can be created, written and removed in it.
bool writableDir(const QString& dir);
// The table of app-ux §10.5а for the program folder and the profile data and cache folders.
StorageLayout storageLayout(const QString& appDir, const QString& profileDataDir, const QString& profileCacheDir);
// For this process: the program folder and the profile folders of QStandardPaths (needs a QCoreApplication).
StorageLayout storageLayout();
// The settings store of a layout: an INI file or the registry.
std::unique_ptr<QSettings> openSettings(const StorageLayout& layout);

}  // namespace construe::app
