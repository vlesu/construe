// Session log and user action trace (spec/app-ux.md §10.8 «Журнал программы», «След действий», FX14-UI-003).
// One module for all records: the session log construe.log (start of the session, files, errors and notifications shown
// to the user, Qt and core messages) and the in-memory ring buffer of the user's actions of the last
// ACTION_TRACE_WINDOW_MS, saved to actions.log for the crash report. Every text goes through sanitizeForLog (the home
// folder → «~»).
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <deque>
#include <functional>

namespace construe::app {

// Trace window and limits (app-ux §10.8 «След действий»).
inline constexpr int64_t ACTION_TRACE_WINDOW_MS = 15 * 60 * 1000;  // 15 minutes
inline constexpr int ACTION_TRACE_MAX = 3000;                      // records at most (older ones drop first)
inline constexpr int ACTION_TRACE_TEXT_MAX = 300;                  // characters of one record at most
inline constexpr int ACTION_TRACE_SAVE_MS = 2000;                  // actions.log is written at most this late

enum class LogLevel { Info, Warning, Error };

// The text with the user's home folder replaced by «~» (both separators; case-insensitive on Windows), line breaks
// replaced by «⏎», at most ACTION_TRACE_TEXT_MAX characters (longer — cut with «…»).
QString sanitizeForLog(const QString& text);

// --- Session log -------------------------------------------------------------------------------------------------

// <data dir>/logs/actions.log and the previous session's actions.prev.log.
QString actionsLogPath(const QString& dataDir);
QString previousActionsLogPath(const QString& dataDir);

// Opens the session log of the data directory: construe.log → construe.prev.log, actions.log → actions.prev.log, a new
// construe.log; the action trace is saved to actions.log from now on; core messages (base/log, info and above) go to the
// log. False — the file could not be opened (the log is off). Also used by tests with a temporary directory.
bool openSessionLog(const QString& dataDir);
// Closes the log (the trace is saved once more); core messages go to stderr again (warn and above).
void closeSessionLog();
bool sessionLogIsOpen();
// One line «<ISO 8601 time with ms> <level> <text>»; the log is off — nothing. Thread-safe.
void logSession(LogLevel level, const QString& text);
// Qt message levels for the Qt message handler (crash_handler): debug, warning, critical, fatal, info.
void logSessionRaw(const char* level, const QString& text);
// The start record: version, build, system, interface language, data directory (OpenGL — from the view at its start).
QString sessionStartText(const QString& dataDir);
void logSessionStart(const QString& dataDir);

// --- Action trace ------------------------------------------------------------------------------------------------

class ActionTrace {
public:
    using Clock = std::function<int64_t()>;  // milliseconds since the epoch
    ActionTrace();
    // Test clock; empty — the system clock.
    void setClock(Clock clock);
    int64_t now() const;
    void add(const QString& text);  // sanitized; records older than the window and beyond the limit drop
    // Records of the last window in time order: «<ISO 8601 time with ms> <text>».
    QStringList lines() const;
    QByteArray text() const;  // lines joined with «\n», a final «\n»; no records — empty
    int size() const;
    void clear();

private:
    void prune(int64_t now) const;
    struct Record {
        int64_t at;
        QString text;
    };
    mutable std::deque<Record> records_;
    Clock clock_;
};

// The program's trace (one per process).
ActionTrace& actionTrace();
// A record of the program's trace; the log is open — actions.log is rewritten within ACTION_TRACE_SAVE_MS.
void traceAction(const QString& text);
// Writes actions.log now (the log is open).
void saveActionTrace();

}  // namespace construe::app
