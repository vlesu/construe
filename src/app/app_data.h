// Application data directory: recent files and autosave (spec/app-ux.md §10.5, S2-DOC-007).
#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

namespace construe::app {

inline constexpr int RECENT_FILES_MAX = 10;
inline constexpr int AUTOSAVE_INTERVAL_MS = 60000;

// Recent files list: path goes first, a duplicate (case-insensitive on Windows) is removed, at most RECENT_FILES_MAX.
QStringList addRecent(QStringList list, const QString& path);
QStringList removeRecent(QStringList list, const QString& path);
// recent.json in directory dir; a corrupted or missing file — an empty list.
QStringList loadRecent(const QString& dir);
bool saveRecent(const QString& dir, const QStringList& list);

struct Recovery {
    QString copyPath;                 // recovery-<pid>.construe
    std::optional<QString> original;  // path of the source document
    QDateTime savedAt;
    qint64 pid = 0;
};

// Document to open from the command line (app-ux §10.1, REL-CT-001): the first argument after the program name that
// does not start with '-'; empty — none.
QString commandLineDocument(const QStringList& arguments);

// Write a copy of process pid's work (atomically) and its metadata.
bool writeRecovery(const QString& dir, qint64 pid, const QByteArray& documentText, const std::optional<QString>& original,
                   const QDateTime& savedAt);
// The most recent copy of another process (pid ≠ ownPid).
std::optional<Recovery> findRecovery(const QString& dir, qint64 ownPid);
void removeRecovery(const QString& dir, qint64 pid);

}  // namespace construe::app
