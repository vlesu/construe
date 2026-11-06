#include "app/crash_handler.h"

#include "app/problem_report.h"
#include "app/session_log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
// windows.h must precede dbghelp.h.
#include <dbghelp.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace construe::app {

namespace {

QtMessageHandler previousHandler = nullptr;

void logHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    static const char* const levels[] = {"debug", "warning", "critical", "fatal", "info"};
    const int t = int(type);
    logSessionRaw(t >= 0 && t < 5 ? levels[t] : "?", msg);
    if (previousHandler) previousHandler(type, ctx, msg);
    else std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
}

// Marker path and text, prepared at installation (the handler must not allocate).
std::string markerText;
#ifdef _WIN32
std::wstring markerPath, dumpPath;  // wide: user folders may contain non-ASCII names
#else
std::string markerPath;
#endif

void writeMarker() {
#ifdef _WIN32
    HANDLE h = CreateFileW(markerPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, markerText.data(), DWORD(markerText.size()), &written, nullptr);
    CloseHandle(h);
#else
    const int fd = ::open(markerPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const ssize_t n = ::write(fd, markerText.data(), markerText.size());
    (void)n;
    ::close(fd);
#endif
}

#ifdef _WIN32
LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info) {
    HANDLE f = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    writeMarker();
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
void signalHandler(int sig) {
    writeMarker();
    std::signal(sig, SIG_DFL);  // the default action of the signal (core, termination)
    std::raise(sig);
}
#endif

}  // namespace

void installLogFile(const QString& dataDir) {
    // The session log (app/session_log): rotation, construe.log and actions.log; Qt messages are added to it.
    if (!openSessionLog(dataDir)) return;
    previousHandler = qInstallMessageHandler(logHandler);
}

void installCrashHandler(const QString& dataDir) {
    if (dataDir.isEmpty()) return;
    QDir().mkpath(crashDir(dataDir));
#ifdef _WIN32
    markerPath = QDir::toNativeSeparators(crashMarkerPath(dataDir)).toStdWString();
    dumpPath = QDir::toNativeSeparators(crashDumpPath(dataDir)).toStdWString();
    const bool dump = true;
#else
    markerPath = QFile::encodeName(crashMarkerPath(dataDir)).toStdString();
    const bool dump = false;
#endif
    markerText = crashMarkerText(QCoreApplication::applicationPid(), QDateTime::currentDateTime(), dump).toStdString();
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandledFilter);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) std::signal(sig, signalHandler);
#endif
}

}  // namespace construe::app
