#include "app/session_log.h"

#include "app/messages.h"
#include "app/problem_report.h"
#include "app/update_check.h"
#include "base/log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>

#include <cstdio>
#include <string>

namespace construe::app {

namespace {

QMutex g_logMutex;
QFile* g_logFile = nullptr;
QString g_actionsPath;     // actions.log of the open log; empty — the trace is not saved
bool g_saveScheduled = false;

QString isoNow() { return QDateTime::currentDateTime().toString(Qt::ISODateWithMs); }

void writeLine(const QByteArray& line) {
    QMutexLocker lock(&g_logMutex);
    if (!g_logFile || !g_logFile->isOpen()) return;
    g_logFile->write(line);
    g_logFile->flush();
}

const char* levelName(LogLevel l) {
    switch (l) {
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "info";
}

void rotate(const QString& current, const QString& previous) {
    QFile::remove(previous);
    QFile::rename(current, previous);
}

}  // namespace

QString sanitizeForLog(const QString& textIn) {
    QString t = textIn;
    const QString home = QDir::homePath();
    if (home.size() > 1) {
#ifdef _WIN32
        const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
        const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
        t.replace(QDir::toNativeSeparators(home), "~", cs);
        t.replace(QDir::fromNativeSeparators(home), "~", cs);
    }
    t.replace("\r\n", "\n");
    t.replace('\n', QChar(0x23CE));  // ⏎: one record — one line
    t.replace('\r', ' ');
    if (t.size() > ACTION_TRACE_TEXT_MAX) t = t.left(ACTION_TRACE_TEXT_MAX - 1) + QChar(0x2026);
    return t;
}

QString actionsLogPath(const QString& dataDir) { return dataDir + "/logs/actions.log"; }
QString previousActionsLogPath(const QString& dataDir) { return dataDir + "/logs/actions.prev.log"; }

bool openSessionLog(const QString& dataDir) {
    closeSessionLog();
    if (dataDir.isEmpty()) return false;
    QDir().mkpath(dataDir + "/logs");
    rotate(logPath(dataDir), previousLogPath(dataDir));
    rotate(actionsLogPath(dataDir), previousActionsLogPath(dataDir));
    auto* f = new QFile(logPath(dataDir));
    if (!f->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete f;
        return false;
    }
    {
        QMutexLocker lock(&g_logMutex);
        g_logFile = f;
        g_actionsPath = actionsLogPath(dataDir);
    }
    // Core messages (base/log, spec/base.md §7): info and above into the log, warn and above also to stderr as before.
    log::setSink([](log::Level level, std::string_view m) {
        if (level < log::Level::Info) return;
        const QString msg = QString::fromUtf8(m.data(), qsizetype(m.size()));
        logSession(level == log::Level::Info ? LogLevel::Info : level == log::Level::Warn ? LogLevel::Warning : LogLevel::Error, msg);
        if (level >= log::Level::Warn) std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    });
    return true;
}

void closeSessionLog() {
    if (!sessionLogIsOpen()) return;
    saveActionTrace();
    log::setSink({});
    QMutexLocker lock(&g_logMutex);
    delete g_logFile;
    g_logFile = nullptr;
    g_actionsPath.clear();
}

bool sessionLogIsOpen() {
    QMutexLocker lock(&g_logMutex);
    return g_logFile != nullptr;
}

void logSessionRaw(const char* level, const QString& text) {
    writeLine(isoNow().toUtf8() + " " + level + " " + sanitizeForLog(text).toUtf8() + "\n");
}

void logSession(LogLevel level, const QString& text) { logSessionRaw(levelName(level), text); }

QString sessionStartText(const QString& dataDir) {
    return QString("session start: construe %1; system %2; Qt %3; language %4; data directory %5")  // i18n-skip: log
        .arg(packageVersion(), reportOs(), QString::fromLatin1(qVersion()), language(), QDir::toNativeSeparators(dataDir));
}

void logSessionStart(const QString& dataDir) {
    logSession(LogLevel::Info, sessionStartText(dataDir));
    traceAction("session start");  // i18n-skip: log
}

// --- ActionTrace -------------------------------------------------------------------------------------------------

ActionTrace::ActionTrace() = default;

void ActionTrace::setClock(Clock clock) { clock_ = std::move(clock); }

int64_t ActionTrace::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }

void ActionTrace::prune(int64_t t) const {
    while (!records_.empty() && (records_.front().at < t - ACTION_TRACE_WINDOW_MS || int(records_.size()) > ACTION_TRACE_MAX))
        records_.pop_front();
}

void ActionTrace::add(const QString& text) {
    const int64_t t = now();
    records_.push_back({t, sanitizeForLog(text)});
    prune(t);
}

QStringList ActionTrace::lines() const {
    prune(now());
    QStringList out;
    for (const Record& r : records_) out << QDateTime::fromMSecsSinceEpoch(r.at).toString(Qt::ISODateWithMs) + " " + r.text;
    return out;
}

QByteArray ActionTrace::text() const {
    const QStringList l = lines();
    return l.isEmpty() ? QByteArray() : l.join('\n').toUtf8() + "\n";
}

int ActionTrace::size() const {
    prune(now());
    return int(records_.size());
}

void ActionTrace::clear() { records_.clear(); }

ActionTrace& actionTrace() {
    static ActionTrace t;
    return t;
}

void saveActionTrace() {
    QString path;
    {
        QMutexLocker lock(&g_logMutex);
        path = g_actionsPath;
    }
    g_saveScheduled = false;
    if (path.isEmpty()) return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(actionTrace().text());
    f.commit();
}

void traceAction(const QString& text) {
    actionTrace().add(text);
    if (g_saveScheduled || !sessionLogIsOpen()) return;
    if (!QCoreApplication::instance()) return saveActionTrace();
    g_saveScheduled = true;
    QTimer::singleShot(ACTION_TRACE_SAVE_MS, QCoreApplication::instance(), [] { saveActionTrace(); });
}

}  // namespace construe::app
