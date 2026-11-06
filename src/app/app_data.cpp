// Application data directory (spec/app-ux.md §10.5).
#include "app/app_data.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace construe::app {

namespace {

bool samePath(const QString& a, const QString& b) {
#if defined(_WIN32) || defined(__APPLE__)  // file systems of Windows and macOS ignore case by default
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
}

bool writeAtomic(const QString& path, const QByteArray& data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(data);
    return f.commit();
}

QString copyName(qint64 pid) { return QString("recovery-%1.construe").arg(pid); }
QString infoName(qint64 pid) { return QString("recovery-%1.json").arg(pid); }

}  // namespace

QStringList addRecent(QStringList list, const QString& path) {
    list = removeRecent(list, path);
    list.prepend(path);
    while (list.size() > RECENT_FILES_MAX) list.removeLast();
    return list;
}

QStringList removeRecent(QStringList list, const QString& path) {
    QStringList out;
    for (const QString& p : list)
        if (!samePath(p, path)) out << p;
    return out;
}

QString commandLineDocument(const QStringList& arguments) {
    for (qsizetype i = 1; i < arguments.size(); ++i)
        if (!arguments[i].isEmpty() && !arguments[i].startsWith('-')) return arguments[i];
    return {};
}

QStringList loadRecent(const QString& dir) {
    QFile f(QDir(dir).filePath("recent.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument j = QJsonDocument::fromJson(f.readAll());
    QStringList out;
    for (const auto& v : j.object().value("recent").toArray())
        if (v.isString() && !v.toString().isEmpty()) out << v.toString();
    while (out.size() > RECENT_FILES_MAX) out.removeLast();
    return out;
}

bool saveRecent(const QString& dir, const QStringList& list) {
    QDir().mkpath(dir);
    QJsonObject o;
    o["recent"] = QJsonArray::fromStringList(list);
    return writeAtomic(QDir(dir).filePath("recent.json"), QJsonDocument(o).toJson());
}

bool writeRecovery(const QString& dir, qint64 pid, const QByteArray& documentText, const std::optional<QString>& original,
                   const QDateTime& savedAt) {
    QDir().mkpath(dir);
    if (!writeAtomic(QDir(dir).filePath(copyName(pid)), documentText)) return false;
    QJsonObject o;
    o["original"] = original ? QJsonValue(*original) : QJsonValue(QJsonValue::Null);
    o["savedAt"] = savedAt.toString(Qt::ISODateWithMs);
    return writeAtomic(QDir(dir).filePath(infoName(pid)), QJsonDocument(o).toJson());
}

std::optional<Recovery> findRecovery(const QString& dir, qint64 ownPid) {
    std::optional<Recovery> best;
    const QDir d(dir);
    for (const QString& name : d.entryList({"recovery-*.json"}, QDir::Files)) {
        bool ok = false;
        const qint64 pid = name.mid(9, name.size() - 9 - 5).toLongLong(&ok);
        if (!ok || pid == ownPid || !QFile::exists(d.filePath(copyName(pid)))) continue;
        QFile f(d.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        Recovery r;
        r.pid = pid;
        r.copyPath = d.filePath(copyName(pid));
        if (o.value("original").isString()) r.original = o.value("original").toString();
        r.savedAt = QDateTime::fromString(o.value("savedAt").toString(), Qt::ISODateWithMs);
        if (!best || r.savedAt > best->savedAt) best = r;
    }
    return best;
}

void removeRecovery(const QString& dir, qint64 pid) {
    QFile::remove(QDir(dir).filePath(copyName(pid)));
    QFile::remove(QDir(dir).filePath(infoName(pid)));
}

}  // namespace construe::app
