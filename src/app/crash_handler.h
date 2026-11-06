// Program log file and crash handler (spec/app-ux.md §10.8, S4-BUG-001, FX14-UI-003): Qt messages go to the session log
// (app/session_log) <data dir>/logs/construe.log
// (the previous session's log is kept as construe.prev.log); an unhandled exception (Windows: minidump) or a fatal signal
// (Linux, macOS) writes the crash marker that the next start offers as a report.
#pragma once

#include <QString>

namespace construe::app {

// Rotate the log (construe.log → construe.prev.log) and send all Qt messages to the new log file and to stderr.
void installLogFile(const QString& dataDir);
// Install the crash handler: the marker text is prepared now (the handler only writes it).
void installCrashHandler(const QString& dataDir);

}  // namespace construe::app
