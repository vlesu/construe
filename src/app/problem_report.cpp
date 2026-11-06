#include "app/messages.h"
#include "app/problem_report.h"

#include "app/update_check.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QUrl>

#include <algorithm>
#include <array>
#include <map>

namespace construe::app {

QString reportUrl() {
    const QByteArray env = qgetenv(REPORT_URL_ENV);
    return env.isEmpty() ? QString::fromLatin1(REPORT_URL) : QString::fromUtf8(env);
}

QString captchaUrl() {
    QUrl url(reportUrl());
    QString path = url.path();
    if (path.endsWith("/reports"))
        path.chop(int(qstrlen("reports")));
    else if (!path.endsWith('/'))
        path += '/';
    url.setPath(path + "captcha");
    return url.toString();
}

QString reportVersion() { return packageVersion(); }

QString reportOs() { return QSysInfo::prettyProductName() + " (" + QSysInfo::currentCpuArchitecture() + ")"; }

bool validEmail(const QString& emailIn) {
    const QString e = emailIn.trimmed();
    if (e.isEmpty() || e.size() > 254) return false;
    for (const QChar c : e)
        if (c.isSpace()) return false;
    if (e.count('@') != 1) return false;
    const qsizetype at = e.indexOf('@');
    const QString domain = e.mid(at + 1);
    if (at < 1 || domain.isEmpty()) return false;
    const qsizetype dot = domain.lastIndexOf('.');
    return domain.indexOf('.') > 0 && dot < domain.size() - 1;
}

bool canSendReport(const QString& description, const QString& email) { return !description.trimmed().isEmpty() && validEmail(email); }

QString uniqueName(const QString& name, const QStringList& names) {
    if (!names.contains(name)) return name;
    const qsizetype dot = name.lastIndexOf('.');
    const QString base = dot > 0 ? name.left(dot) : name, ext = dot > 0 ? name.mid(dot) : QString();
    for (int i = 2;; ++i) {
        const QString n = base + "-" + QString::number(i) + ext;
        if (!names.contains(n)) return n;
    }
}

namespace {

QByteArray imageType(const QString& name) {
    const QString l = name.toLower();
    return l.endsWith(".png") ? "image/png" : "image/jpeg";
}

// Quoted header parameter value (RFC 7578: quotes and line breaks escaped).
QByteArray quoted(const QString& s) {
    QByteArray b = s.toUtf8();
    b.replace("\\", "\\\\").replace("\"", "\\\"").replace("\r", "%0D").replace("\n", "%0A");
    return "\"" + b + "\"";
}

}  // namespace

QByteArray reportBody(const ProblemReport& r, const QByteArray& boundary) {
    QByteArray out;
    auto field = [&](const char* name, const QString& value) {
        out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value.toUtf8() + "\r\n";
    };
    auto file = [&](const char* name, const ReportFile& f, const QByteArray& type) {
        out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=" + quoted(f.name) +
               "\r\nContent-Type: " + type + "\r\n\r\n";
        out += f.data;
        out += "\r\n";
    };
    field("description", r.description.trimmed());
    field("email", r.email.trimmed());
    field("version", r.version);
    field("os", r.os);
    field("kind", r.kind);
    for (const auto& f : r.screenshots) file("screenshots[]", f, imageType(f.name));
    for (const auto& f : r.attachments) file("attachments[]", f, "application/octet-stream");
    out += "--" + boundary + "--\r\n";
    return out;
}

QByteArray reportContentType(const QByteArray& boundary) { return "multipart/form-data; boundary=" + boundary; }

QByteArray newBoundary() {
    QByteArray b = "construe-";
    for (int i = 0; i < 4; ++i) b += QByteArray::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, '0');
    return b;
}

QByteArray reportJson(const ProblemReport& r, const QDateTime& created) {
    QJsonArray shots, atts;
    for (const auto& f : r.screenshots) shots.append(f.name);
    for (const auto& f : r.attachments) atts.append(f.name);
    QJsonObject o{{"description", r.description.trimmed()},
                  {"email", r.email.trimmed()},
                  {"version", r.version},
                  {"os", r.os},
                  {"kind", r.kind},
                  {"created", created.toUTC().toString(Qt::ISODate)},
                  {"screenshots", shots},
                  {"attachments", atts}};
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

namespace {

const std::array<quint32, 256>& crcTable() {
    static const std::array<quint32, 256> t = [] {
        std::array<quint32, 256> a{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            a[i] = c;
        }
        return a;
    }();
    return t;
}
quint32 crc32(const QByteArray& d) {
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : d) c = crcTable()[(c ^ quint8(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)).append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v) { put16(b, quint16(v & 0xFFFF)), put16(b, quint16(v >> 16)); }
quint16 get16(const QByteArray& b, qsizetype i) { return quint16(quint8(b[i]) | (quint8(b[i + 1]) << 8)); }
quint32 get32(const QByteArray& b, qsizetype i) { return quint32(get16(b, i)) | (quint32(get16(b, i + 2)) << 16); }

}  // namespace

QByteArray reportZip(const ProblemReport& r, const QDateTime& created) {
    std::vector<ReportFile> entries{{"report.json", reportJson(r, created)}};
    for (const auto& f : r.screenshots) entries.push_back({"screenshots/" + f.name, f.data});
    for (const auto& f : r.attachments) entries.push_back({"attachments/" + f.name, f.data});
    // DOS time and date of the archive creation (local time).
    const QDateTime t = created.toLocalTime();
    const quint16 dosTime = quint16((t.time().hour() << 11) | (t.time().minute() << 5) | (t.time().second() / 2));
    const quint16 dosDate = quint16(((std::max(1980, t.date().year()) - 1980) << 9) | (t.date().month() << 5) | t.date().day());
    QByteArray out, central;
    for (const auto& e : entries) {
        const QByteArray name = e.name.toUtf8();
        const quint32 crc = crc32(e.data), size = quint32(e.data.size()), offset = quint32(out.size());
        put32(out, 0x04034b50);
        put16(out, 20), put16(out, 0x0800), put16(out, 0), put16(out, dosTime), put16(out, dosDate);
        put32(out, crc), put32(out, size), put32(out, size);
        put16(out, quint16(name.size())), put16(out, 0);
        out += name;
        out += e.data;
        put32(central, 0x02014b50);
        put16(central, 20), put16(central, 20), put16(central, 0x0800), put16(central, 0), put16(central, dosTime), put16(central, dosDate);
        put32(central, crc), put32(central, size), put32(central, size);
        put16(central, quint16(name.size())), put16(central, 0), put16(central, 0), put16(central, 0), put16(central, 0);
        put32(central, 0), put32(central, offset);
        central += name;
    }
    const quint32 cdOffset = quint32(out.size()), cdSize = quint32(central.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0), put16(out, 0), put16(out, quint16(entries.size())), put16(out, quint16(entries.size()));
    put32(out, cdSize), put32(out, cdOffset), put16(out, 0);
    return out;
}

std::optional<std::vector<ReportFile>> readStoredZip(const QByteArray& zip) {
    std::vector<ReportFile> out;
    qsizetype i = 0;
    while (i + 30 <= zip.size() && get32(zip, i) == 0x04034b50) {
        if (get16(zip, i + 8) != 0) return std::nullopt;  // only stored entries
        const quint32 crc = get32(zip, i + 14), size = get32(zip, i + 18);
        const quint16 nameLen = get16(zip, i + 26), extraLen = get16(zip, i + 28);
        const qsizetype data = i + 30 + nameLen + extraLen;
        if (data + qsizetype(size) > zip.size()) return std::nullopt;
        ReportFile f{QString::fromUtf8(zip.mid(i + 30, nameLen)), zip.mid(data, size)};
        if (crc32(f.data) != crc) return std::nullopt;
        out.push_back(std::move(f));
        i = data + size;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

ReportReply parseReportReply(int httpStatus, const QByteArray& body, const QString& networkError) {
    ReportReply r;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    const QString status = d.isObject() ? d.object().value("status").toString() : QString();
    if (status == "received" && httpStatus >= 200 && httpStatus < 300) {
        r.kind = ReportReply::Kind::Success;
        r.id = d.object().value("id").toString();
        return r;
    }
    if (status == "error") {
        r.kind = ReportReply::Kind::Refused;
        r.error = d.object().value("error").toString();
        r.message = d.object().value("message").toString();
        r.field = d.object().value("field").toString();
        r.file = d.object().value("file").toString();
        const QJsonValue limit = d.object().value("limit");
        if (limit.isDouble() && limit.toDouble() >= 0) r.limit = static_cast<qint64>(limit.toDouble());
        for (const QJsonValue& v : d.object().value("allowed").toArray())
            if (v.isString()) r.allowed << v.toString();
        return r;
    }
    r.kind = ReportReply::Kind::Offline;
    r.detail = !networkError.isEmpty() ? networkError : httpStatus > 0 ? "HTTP " + QString::number(httpStatus) : QString(txt("ui.report.noResponse"));
    return r;
}

CaptchaReply parseCaptchaReply(int httpStatus, const QByteArray& body, const QString& networkError) {
    CaptchaReply c;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    const QJsonObject o = d.isObject() ? d.object() : QJsonObject();
    if (httpStatus >= 200 && httpStatus < 300) {
        const QString token = o.value("token").toString();
        const QByteArray png = QByteArray::fromBase64(o.value("image").toString().toLatin1());
        if (!token.isEmpty() && png.startsWith("\x89PNG\r\n\x1a\n")) {
            c.kind = CaptchaReply::Kind::Success;
            c.token = token;
            c.png = png;
            const QJsonValue ttl = o.value("expires_in");
            if (ttl.isDouble() && ttl.toDouble() > 0) c.expiresInSec = static_cast<int>(ttl.toDouble());
            return c;
        }
    }
    // An error or no connection: the same reading as a report reply.
    c.refusal = parseReportReply(httpStatus, o.value("status").toString() == "error" ? body : QByteArray(), networkError);
    if (c.refusal.kind == ReportReply::Kind::Refused) {
        c.kind = CaptchaReply::Kind::Refused;
    } else {
        c.kind = CaptchaReply::Kind::Offline;
        c.refusal.kind = ReportReply::Kind::Offline;
    }
    return c;
}

QString captchaFailureText(const CaptchaReply& c) {
    return text("ui.report.captchaFailed",
                {{"detail", c.kind == CaptchaReply::Kind::Refused ? reportErrorText(c.refusal) : c.refusal.detail}});
}

namespace {

// Label of a form field in the window (§10.8); a field without one — its name as is.
QString reportFieldLabel(const QString& field) {
    if (field == "description") return text("ui.report.problemDescription");
    if (field == "email") return text("ui.report.eMailForReply");
    if (field == "screenshots") return text("ui.report.screenshots");
    return field;
}

// Length limits of the receiver known to the client (app-ux §10.8), for a reply without "limit".
qint64 knownLengthLimit(const QString& field) {
    if (field == "description") return 20000;
    if (field == "email") return 254;
    if (field == "kind") return 20;
    return 200;
}

}  // namespace

QString reportErrorText(const ReportReply& r) {
    const QString& code = r.error;
    auto fieldFromCode = [&](const char* prefix) { return r.field.isEmpty() ? code.mid(int(qstrlen(prefix))) : r.field; };
    auto num = [](qint64 v) { return QString::number(v); };
    if (code.startsWith("missing_") && code.size() > 8)
        return text("ui.report.err.missingField", {{"field", reportFieldLabel(fieldFromCode("missing_"))}});
    if (code.startsWith("too_long_") && code.size() > 9) {
        const QString field = fieldFromCode("too_long_");
        return text("ui.report.err.tooLong",
                    {{"field", reportFieldLabel(field)}, {"limit", num(r.limit >= 0 ? r.limit : knownLengthLimit(field))}});
    }
    if (code == "too_large")
        return text("ui.report.err.tooLarge", {{"limit", num((r.limit >= 0 ? r.limit : REPORT_MAX_BYTES) / (1024 * 1024))}});
    if (code == "rate_limited")
        return r.limit >= 0 ? text("ui.report.err.rateLimited", {{"limit", num(r.limit)}}) : text("ui.report.err.rateLimitedNoLimit");
    if (code == "captcha_rate_limited")
        return r.limit >= 0 ? text("ui.report.err.captchaRateLimited", {{"limit", num(r.limit)}})
                            : text("ui.report.err.captchaRateLimitedNoLimit");
    static const std::map<QString, const char*> plain = {
        {"bad_content_type", "ui.report.err.badContentType"}, {"length_required", "ui.report.err.lengthRequired"},
        {"bad_length", "ui.report.err.badLength"}, {"bad_multipart", "ui.report.err.badMultipart"},
        {"bad_email", "ui.report.err.badEmail"}, {"bad_kind", "ui.report.err.badKind"},
        {"not_found", "ui.report.err.notFound"}, {"internal_error", "ui.report.err.internal"},
        {"unavailable", "ui.report.err.unavailable"}, {"captcha_required", "ui.report.err.captchaRequired"},
        {"captcha_wrong", "ui.report.err.captchaWrong"}, {"captcha_expired", "ui.report.err.captchaExpired"},
        {"captcha_used", "ui.report.err.captchaUsed"}};
    if (auto it = plain.find(code); it != plain.end()) return text(it->second);
    if (!r.field.isEmpty()) {
        static const std::map<QString, const char*> withField = {
            {"duplicate_field", "ui.report.err.duplicateField"}, {"bad_field", "ui.report.err.badField"},
            {"bad_file", "ui.report.err.badFile"}};
        if (auto it = withField.find(code); it != withField.end()) return text(it->second, {{"field", reportFieldLabel(r.field)}});
        if (code == "too_many_files")
            return text("ui.report.err.tooManyFiles",
                        {{"field", reportFieldLabel(r.field)}, {"limit", num(r.limit >= 0 ? r.limit : REPORT_MAX_FILES)}});
    }
    if (!r.file.isEmpty()) {
        if (code == "bad_screenshot") return text("ui.report.err.badScreenshot", {{"file", r.file}});
        if (code == "bad_attachment" && !r.allowed.isEmpty())
            return text("ui.report.err.badAttachment", {{"file", r.file}, {"allowed", r.allowed.join(", ")}});
    }
    // An unknown code or a missing substitution: the generic text with the server's own message as the detail.
    if (r.message.isEmpty()) return text("ui.report.err.unknownCode", {{"code", code}});
    return text("ui.report.err.unknown", {{"code", code}, {"detail", r.message}});
}

QString crashDir(const QString& dataDir) { return dataDir + "/crash"; }
QString crashMarkerPath(const QString& dataDir) { return crashDir(dataDir) + "/crash.json"; }
QString crashDumpPath(const QString& dataDir) { return crashDir(dataDir) + "/crash.dmp"; }

QByteArray crashMarkerText(qint64 pid, const QDateTime& started, bool dump) {
    return "{\"pid\": " + QByteArray::number(pid) + ", \"started\": \"" + started.toUTC().toString(Qt::ISODate).toLatin1() +
           "\", \"dump\": " + (dump ? "true" : "false") + "}\n";
}

bool writeCrashMarker(const QString& dataDir, const CrashMarker& m) {
    QDir().mkpath(crashDir(dataDir));
    QFile f(crashMarkerPath(dataDir));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QJsonObject o{{"pid", double(m.pid)}, {"started", m.started}, {"dump", m.dump}};
    return f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n") > 0;
}

std::optional<CrashMarker> readCrashMarker(const QString& dataDir) {
    if (dataDir.isEmpty()) return std::nullopt;
    QFile f(crashMarkerPath(dataDir));
    if (!f.exists()) return std::nullopt;
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!d.isObject() || !d.object().value("pid").isDouble()) {
        clearCrashReport(dataDir);  // an unreadable marker is removed without a window
        return std::nullopt;
    }
    CrashMarker m;
    m.pid = qint64(d.object().value("pid").toDouble());
    m.started = d.object().value("started").toString();
    m.dump = d.object().value("dump").toBool() && QFileInfo::exists(crashDumpPath(dataDir));
    return m;
}

void clearCrashReport(const QString& dataDir) {
    if (dataDir.isEmpty()) return;
    QFile::remove(crashMarkerPath(dataDir));
    QFile::remove(crashDumpPath(dataDir));
}

QString logPath(const QString& dataDir) { return dataDir + "/logs/construe.log"; }
QString previousLogPath(const QString& dataDir) { return dataDir + "/logs/construe.prev.log"; }

}  // namespace construe::app
