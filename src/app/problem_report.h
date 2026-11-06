// Problem report (spec/app-ux.md §10.8, S4-BUG-001) without widgets: the request body for the report receiver
// (multipart/form-data), field checks, the zip archive for manual sending, the server reply and the crash marker.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace construe::app {

// Constants of §10.8.
inline constexpr const char* REPORT_URL = "https://construe.vlesu.com/api/v1/reports";
inline constexpr const char* REPORT_URL_ENV = "CONSTRUE_REPORT_URL";
inline constexpr qint64 REPORT_MAX_BYTES = 50LL * 1024 * 1024;
inline constexpr int REPORT_MAX_FILES = 20;
inline constexpr int REPORT_TIMEOUT_MS = 60000;
// Captcha (§10.8 «Капча», FX14-SEC-001): request headers of the token and the answer.
inline constexpr const char* CAPTCHA_HEADER = "X-Construe-Captcha";
inline constexpr const char* CAPTCHA_ANSWER_HEADER = "X-Construe-Captcha-Answer";
inline constexpr int CAPTCHA_DEFAULT_TTL_SEC = 600;

struct ReportFile {
    QString name;  // file name in the request and in the zip
    QByteArray data;
};

struct ProblemReport {
    QString description, email, version, os;
    QString kind = "manual";  // manual | crash
    std::vector<ReportFile> screenshots, attachments;
};

// The receiver address: CONSTRUE_REPORT_URL if set, otherwise REPORT_URL.
QString reportUrl();
// The captcha picture address: reportUrl() with the last path segment "reports" replaced by "captcha".
QString captchaUrl();
// Package version (update.md §2) and system (§10.8 table).
QString reportVersion();
QString reportOs();

// E-mail check (§10.8): trimmed, ≤ 254 characters, no whitespace, one '@', a domain with an inner dot.
bool validEmail(const QString& email);
// "Send" is allowed: a non-empty trimmed description and a valid e-mail.
bool canSendReport(const QString& description, const QString& email);
// Unique file name within names: "-2", "-3", … before the extension.
QString uniqueName(const QString& name, const QStringList& names);

// multipart/form-data body (parts in the order of the §10.8 table) and its Content-Type for boundary.
QByteArray reportBody(const ProblemReport& r, const QByteArray& boundary);
QByteArray reportContentType(const QByteArray& boundary);
QByteArray newBoundary();  // "construe-" + 32 hex digits

// report.json of the zip (§10.8): fields, created (ISO 8601 UTC), file names.
QByteArray reportJson(const ProblemReport& r, const QDateTime& created);
// Stored zip (method 0, UTF-8 names): report.json, screenshots/<name>, attachments/<name>.
QByteArray reportZip(const ProblemReport& r, const QDateTime& created);
// Entries of a stored zip (name, data) — for checks; nullopt — not such an archive or a CRC mismatch.
std::optional<std::vector<ReportFile>> readStoredZip(const QByteArray& zip);

// Server reply (§10.8): success (id), refusal (error, message and the optional substitutions field, limit, file,
// allowed) or no connection (detail).
struct ReportReply {
    enum class Kind { Success, Refused, Offline } kind = Kind::Offline;
    QString id, error, message, detail;
    QString field, file;
    QStringList allowed;
    qint64 limit = -1;  // -1 — absent
};
// httpStatus 0 — no HTTP reply; networkError — the network error text (empty if none).
ReportReply parseReportReply(int httpStatus, const QByteArray& body, const QString& networkError = {});
// Text of a server refusal in the interface language by its error code (§10.8 table, S8-EN-001); missing
// substitutions — values known to the client; an unknown code — ui.report.err.unknown with the server message.
QString reportErrorText(const ReportReply& r);

// Captcha picture reply (§10.8 «Капча»): a picture (token, PNG, lifetime) or a refusal / no connection with the reply
// read as a report reply (refusal: error code and substitutions; offline: detail).
struct CaptchaReply {
    enum class Kind { Success, Refused, Offline } kind = Kind::Offline;
    QString token;
    QByteArray png;
    int expiresInSec = CAPTCHA_DEFAULT_TTL_SEC;
    ReportReply refusal;
};
CaptchaReply parseCaptchaReply(int httpStatus, const QByteArray& body, const QString& networkError = {});
// ui.report.captchaFailed with the refusal text (reportErrorText) or the network detail.
QString captchaFailureText(const CaptchaReply& c);

// Crash marker (§10.8) in <data dir>/crash: crash.json and crash.dmp.
struct CrashMarker {
    qint64 pid = 0;
    QString started;
    bool dump = false;
};
QString crashDir(const QString& dataDir);
QString crashMarkerPath(const QString& dataDir);
QString crashDumpPath(const QString& dataDir);
// Marker text prepared in advance (the signal handler only writes it).
QByteArray crashMarkerText(qint64 pid, const QDateTime& started, bool dump);
bool writeCrashMarker(const QString& dataDir, const CrashMarker& m);
// The marker if present and readable; an unreadable marker is removed (nullopt).
std::optional<CrashMarker> readCrashMarker(const QString& dataDir);
// Removes the marker and the dump: the crash is no longer offered.
void clearCrashReport(const QString& dataDir);

// Program log (§10.8) in <data dir>/logs.
QString logPath(const QString& dataDir);
QString previousLogPath(const QString& dataDir);

}  // namespace construe::app
