#include "app/portable.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace construe::app {

bool portableMarkerPresent(const QString& appDir) {
    return !appDir.isEmpty() && QFileInfo(QDir(appDir).filePath(PORTABLE_MARKER_FILE)).isFile();
}

bool writableDir(const QString& dir) {
    if (dir.isEmpty() || !QDir().mkpath(dir)) return false;
    // QFileInfo::isWritable is unreliable for folders on Windows (ACLs, read-only media): try a real file.
    QFile probe(QDir(dir).filePath(".write-test"));
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = probe.write("construe", 8) == 8;
    probe.close();
    return probe.remove() && written;
}

StorageLayout storageLayout(const QString& appDir, const QString& profileDataDir, const QString& profileCacheDir) {
    StorageLayout l;
    l.dataDir = profileDataDir;
    l.cacheDir = profileCacheDir;
    l.portable = portableMarkerPresent(appDir);
    if (!l.portable) return l;
    const QString beside = QDir(appDir).filePath(PORTABLE_DATA_DIR);
    if (writableDir(beside)) {
        l.beside = true;
        l.dataDir = beside;
        l.cacheDir = QDir(beside).filePath("cache");
    }
    l.settingsFile = QDir(l.dataDir).filePath(PORTABLE_SETTINGS_FILE);
    return l;
}

StorageLayout storageLayout() {
    // Once per process: the log, the crash handler, the settings and the window must agree on the places.
    static const StorageLayout layout = storageLayout(QCoreApplication::applicationDirPath(), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
                                                      QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    return layout;
}

std::unique_ptr<QSettings> openSettings(const StorageLayout& layout) {
    if (layout.settingsFile.isEmpty()) return std::make_unique<QSettings>("construe", "construe");
    QDir().mkpath(QFileInfo(layout.settingsFile).absolutePath());
    return std::make_unique<QSettings>(layout.settingsFile, QSettings::IniFormat);
}

}  // namespace construe::app
