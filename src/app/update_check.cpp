#include "app/update_check.h"

#include "app/messages.h"

#include "construe_build_info.h"  // generated: build number, commit, update system, package format (cmake/build_info.cmake)

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimeZone>

#include <cmath>
#include <tuple>
#include <cstdio>

namespace construe::app {

namespace {

std::function<bool(UpdateAction, const QString&)>& testAction() {
    static std::function<bool(UpdateAction, const QString&)> f;
    return f;
}

struct TestTarget {
    QString os;
    PackageFormat format = PackageFormat::None;
};
TestTarget& testTarget() {
    static TestTarget t;
    return t;
}

bool& portableMode() {
    static bool portable = false;
    return portable;
}

// A JSON number that is an integer in [lo, hi] (spec/update.md §3: no fractional part).
std::optional<qint64> integerIn(const QJsonValue& v, double lo, double hi) {
    if (!v.isDouble()) return std::nullopt;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d != std::floor(d) || d < lo || d > hi) return std::nullopt;
    return qint64(d);
}

// url, sha256 and size of a package (§3 rules 5–7); nullopt — any rule broken.
std::optional<PackageRef> packageRef(const QJsonObject& o, const QUrl& base) {
    const QString url = o.value("url").toString();
    if (url.isEmpty()) return std::nullopt;
    const QUrl ref(url, QUrl::StrictMode);
    if (!ref.isValid()) return std::nullopt;
    const QUrl resolved = base.resolved(ref);
    const QString scheme = resolved.scheme().toLower();
    if (!resolved.isValid() || (scheme != "https" && scheme != "http") || resolved.host().isEmpty()) return std::nullopt;
    static const QRegularExpression hex("^[0-9A-Fa-f]{64}$");
    const QString sha = o.value("sha256").toString();
    if (!hex.match(sha).hasMatch()) return std::nullopt;
    const auto size = integerIn(o.value("size"), 1, double(UPDATE_MAX_INSTALLER_BYTES));
    if (!size) return std::nullopt;
    return PackageRef{resolved, sha.toLower().toLatin1(), *size};
}

}  // namespace

std::strong_ordering comparePrerelease(const QString& a, const QString& b) {
    if (a == b) return std::strong_ordering::equal;
    if (a.isEmpty()) return std::strong_ordering::greater;  // a release follows its pre-releases
    if (b.isEmpty()) return std::strong_ordering::less;
    const QStringList x = a.split('.'), y = b.split('.');
    static const QRegularExpression numeric("^[0-9]+$");
    for (qsizetype i = 0; i < x.size() && i < y.size(); ++i) {
        if (x[i] == y[i]) continue;
        const bool nx = numeric.match(x[i]).hasMatch(), ny = numeric.match(y[i]).hasMatch();
        if (nx && ny) {
            // No leading zeros (validPrerelease): a longer number is greater, equal lengths compare as text.
            if (x[i].size() != y[i].size()) return x[i].size() <=> y[i].size();
            return x[i].compare(y[i]) < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        if (nx != ny) return nx ? std::strong_ordering::less : std::strong_ordering::greater;
        return x[i].toLatin1() < y[i].toLatin1() ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return x.size() <=> y.size();
}

bool validPrerelease(const QString& label) {
    static const QRegularExpression re("^[A-Za-z][0-9A-Za-z]*(\\.(0|[1-9][0-9]*|[0-9]*[A-Za-z][0-9A-Za-z]*))*$");
    return !label.isEmpty() && label.size() <= PRERELEASE_MAX_CHARS && re.match(label).hasMatch();
}

std::strong_ordering AppVersion::operator<=>(const AppVersion& other) const {
    if (auto c = std::tie(major, minor, patch) <=> std::tie(other.major, other.minor, other.patch); c != 0) return c;
    if (auto c = comparePrerelease(prerelease, other.prerelease); c != 0) return c;
    return build <=> other.build;
}

AppVersion currentVersion() {
    AppVersion v = parseVersion(QString::fromLatin1(CONSTRUE_VERSION)).value_or(AppVersion{});
    const QString label = QString::fromLatin1(CONSTRUE_VERSION_PRERELEASE);
    if (validPrerelease(label)) v.prerelease = label;
    v.build = qint64(CONSTRUE_BUILD_NUMBER);
    return v;
}

QString currentCommit() { return QString::fromLatin1(CONSTRUE_COMMIT); }

std::optional<AppVersion> parseVersion(const QString& text) {
    static const QRegularExpression re("^(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})(?:-(.+))?$");
    const auto m = re.match(text);
    if (!m.hasMatch()) return std::nullopt;
    AppVersion v;
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).toInt();
    v.patch = m.captured(3).toInt();
    if (m.hasCaptured(4)) {
        if (!validPrerelease(m.captured(4))) return std::nullopt;
        v.prerelease = m.captured(4);
    }
    return v;
}

QString versionString(const AppVersion& v) {
    QString s = QString("%1.%2.%3").arg(v.major).arg(v.minor).arg(v.patch);
    if (!v.prerelease.isEmpty()) s += "-" + v.prerelease;
    return s;
}

QDateTime buildDateTime(qint64 build) {
    if (build < 100000000000LL || build > BUILD_NUMBER_MAX) return {};
    const int minute = int(build % 100), hour = int(build / 100 % 100);
    const int day = int(build / 10000 % 100), month = int(build / 1000000 % 100), year = int(build / 100000000);
    if (year < 2000 || minute > 59 || hour > 23) return {};
    const QDate date(year, month, day);
    if (!date.isValid()) return {};
    return QDateTime(date, QTime(hour, minute), QTimeZone::UTC);
}

QString buildTag(qint64 build) {
    const QDateTime t = buildDateTime(build);
    return t.isValid() ? t.toString("yyyyMMdd-HHmm") : QString::number(build);
}

QString versionText(const AppVersion& v) {
    const QString xyz = versionString(v);
    if (v.build <= 0) return xyz;
    const QDateTime t = buildDateTime(v.build);
    const QString build = t.isValid() ? t.toString("yyyy-MM-dd HH:mm") + " UTC" : QString::number(v.build);
    return text("ui.update.versionBuild", {{"version", xyz}, {"build", build}});
}

QString packageVersion(const AppVersion& v, const QString& commit) {
    QString s = versionString(v) + "-" + buildTag(v.build);
    if (!commit.isEmpty()) s += "-" + commit;
    return s;
}

QString packageVersion() { return packageVersion(currentVersion(), currentCommit()); }

QString updateOs() {
    if (!testTarget().os.isEmpty()) return testTarget().os;
    return QString::fromLatin1(CONSTRUE_UPDATE_OS);
}

PackageFormat packageFormat() {
    if (!testTarget().os.isEmpty()) return testTarget().format;
    const PackageFormat built = parsePackageFormat(QString::fromLatin1(CONSTRUE_PACKAGE_FORMAT)).value_or(PackageFormat::None);
    // A setup build running without installation updates with the zip (§2а, S8-PKG-001).
    return built == PackageFormat::Setup && portableMode() ? PackageFormat::Zip : built;
}

void setPortableMode(bool portable) { portableMode() = portable; }

std::optional<PackageFormat> parsePackageFormat(const QString& name) {
    if (name == "setup") return PackageFormat::Setup;
    if (name == "appimage") return PackageFormat::AppImage;
    if (name == "flatpak") return PackageFormat::Flatpak;
    if (name == "dmg") return PackageFormat::Dmg;
    if (name == "none") return PackageFormat::None;
    if (name == "zip") return PackageFormat::Zip;
    return std::nullopt;
}

QString packageFormatName(PackageFormat f) {
    switch (f) {
        case PackageFormat::Setup: return "setup";
        case PackageFormat::AppImage: return "appimage";
        case PackageFormat::Flatpak: return "flatpak";
        case PackageFormat::Dmg: return "dmg";
        case PackageFormat::Zip: return "zip";
        case PackageFormat::None: break;
    }
    return "none";
}

bool formatFitsOs(PackageFormat f, const QString& os) {
    if (os != "windows" && os != "linux" && os != "macos") return false;
    switch (f) {
        case PackageFormat::Setup:
        case PackageFormat::Zip: return os == "windows";
        case PackageFormat::AppImage:
        case PackageFormat::Flatpak: return os == "linux";
        case PackageFormat::Dmg: return os == "macos";
        case PackageFormat::None: break;
    }
    return true;
}

void setUpdateTargetForTest(const QString& os, PackageFormat format) { testTarget() = {os, format}; }

bool formatDownloads(PackageFormat f) {
    return f == PackageFormat::Setup || f == PackageFormat::AppImage || f == PackageFormat::Dmg || f == PackageFormat::Zip;
}

QString flatpakUpdateCommand() { return QString("flatpak update ") + FLATPAK_APP_ID; }

QString updateBaseUrl() {
    const QByteArray env = qgetenv(UPDATE_URL_ENV);
    QString base = env.isEmpty() ? QString::fromLatin1(UPDATE_BASE_URL) : QString::fromUtf8(env);
    if (!base.endsWith('/')) base += '/';
    return base;
}

QString updateUrl(const QString& folder) { return updateBaseUrl() + folder + "/latest.json"; }

QString updateUrl() { return updateUrl(updateFolder(updateOs(), buildArch())); }

QString updateFolder(const QString& os, const QString& arch) { return os == "macos" ? "macos-" + arch : os; }

QString buildArch() {
    const QString a = QSysInfo::buildCpuArchitecture();
    return a == "aarch64" ? QString("arm64") : a;  // Qt names Apple Silicon "arm64"; Linux on ARM — "arm64" as well
}

bool updateUrlOverridden() { return !qgetenv(UPDATE_URL_ENV).isEmpty(); }

std::optional<UpdateInfo> parseUpdateInfo(const QByteArray& body, const QUrl& base) {
    if (body.size() > UPDATE_INFO_MAX_BYTES) return std::nullopt;
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !d.isObject()) return std::nullopt;
    const QJsonObject o = d.object();
    if (o.value("format").toString() != "construe-update") return std::nullopt;
    if (integerIn(o.value("formatVersion"), UPDATE_FORMAT_VERSION, UPDATE_FORMAT_VERSION) != UPDATE_FORMAT_VERSION) return std::nullopt;
    if (!o.value("version").isString()) return std::nullopt;
    // Rule 3: version is x.y.z only; the pre-release label is the separate optional field (rule 3а) — clients before it
    // ignore the field and still read the description.
    auto version = parseVersion(o.value("version").toString());
    const auto build = integerIn(o.value("build"), 1, double(BUILD_NUMBER_MAX));
    if (!version || !version->prerelease.isEmpty() || !build) return std::nullopt;
    const QJsonValue pre = o.value("prerelease");
    if (!pre.isUndefined()) {
        if (!pre.isString() || (!pre.toString().isEmpty() && !validPrerelease(pre.toString()))) return std::nullopt;
        version->prerelease = pre.toString();
    }
    version->build = *build;
    const auto package = packageRef(o, base);
    if (!package) return std::nullopt;
    const QJsonValue notes = o.value("notes");
    if (!notes.isUndefined() && !notes.isString()) return std::nullopt;
    UpdateInfo info;
    info.version = *version;
    info.url = package->url;
    info.sha256 = package->sha256;
    info.size = package->size;
    info.notes = notes.toString();
    // The optional zip (rule 9): an invalid one is absent, the description stays valid.
    if (o.value("zip").isObject()) info.zip = packageRef(o.value("zip").toObject(), base);
    return info;
}

std::optional<UpdateInfo> offerFor(const UpdateInfo& info, PackageFormat format) {
    if (format != PackageFormat::Zip) return info;
    if (!info.zip) return std::nullopt;
    UpdateInfo zip = info;
    zip.url = info.zip->url;
    zip.sha256 = info.zip->sha256;
    zip.size = info.zip->size;
    return zip;
}

QString displayNotes(const QString& notes) {
    const QString t = notes.trimmed();
    return t.size() > UPDATE_NOTES_MAX_CHARS ? t.left(UPDATE_NOTES_MAX_CHARS) + QString::fromUtf8("…") : t;
}

bool updateCheckDue(const QString& lastCheck, const QDateTime& now) {
    if (lastCheck.isEmpty()) return true;
    const QDateTime last = QDateTime::fromString(lastCheck, Qt::ISODate);
    if (!last.isValid()) return true;
    const qint64 ago = last.secsTo(now);
    return ago < 0 || ago >= UPDATE_CHECK_INTERVAL_S;
}

QString updateCheckStamp(const QDateTime& now) { return now.toUTC().toString(Qt::ISODate); }

QString packageFileName(const UpdateInfo& info, PackageFormat format) {
    static const QRegularExpression safe("^[A-Za-z0-9._-]{1,128}$");
    const AppVersion& v = info.version;
    const QString xyzb = versionString(v) + "-" + buildTag(v.build);
    QString ext = ".exe", fallback = "construe-setup-" + xyzb + ".exe";
    if (format == PackageFormat::AppImage) {
        ext = ".AppImage";
        fallback = "construe-" + xyzb + "-x86_64.AppImage";
    } else if (format == PackageFormat::Dmg) {
        ext = ".dmg";
        fallback = "construe-" + xyzb + ".dmg";
    } else if (format == PackageFormat::Zip) {
        ext = ".zip";
        fallback = "construe-" + xyzb + "-win64.zip";
    }
    const QString name = info.url.fileName();
    if (safe.match(name).hasMatch() && name.endsWith(ext, Qt::CaseInsensitive) && name.compare(ext, Qt::CaseInsensitive) != 0) return name;
    return fallback;
}

QString defaultUpdateDownloadDir() { return QDir::tempPath() + "/construe-update"; }

QString defaultUpdateSaveDir() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

QString replaceableAppImage() {
    const QString path = QString::fromLocal8Bit(qgetenv(APPIMAGE_ENV));
    if (path.isEmpty()) return {};
    const QFileInfo file(path);
    const QFileInfo dir(file.absolutePath());
    if (!file.isFile() || !dir.isDir() || !dir.isWritable()) return {};
    return file.absoluteFilePath();
}

bool makeExecutable(const QString& path) {
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                           QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

bool replaceAppImage(const QString& downloaded, const QString& target) {
    if (!makeExecutable(downloaded)) return false;
    // std::rename replaces an existing target atomically on POSIX (QFile::rename refuses to); AppImage is Linux-only.
    return std::rename(QFile::encodeName(downloaded).constData(), QFile::encodeName(target).constData()) == 0;
}

LaunchResult runUpdateAction(UpdateAction action, const QString& arg) {
    if (testAction()) return testAction()(action, arg) ? LaunchResult::Launched : LaunchResult::Failed;
    auto ok = [](bool b) { return b ? LaunchResult::Launched : LaunchResult::Failed; };
    switch (action) {
        case UpdateAction::RunInstaller:
#ifdef Q_OS_WIN
            return ok(QProcess::startDetached(QDir::toNativeSeparators(arg), {}));
#else
            return LaunchResult::Unsupported;
#endif
        case UpdateAction::OpenDiskImage: return ok(QDesktopServices::openUrl(QUrl::fromLocalFile(arg)));
        case UpdateAction::RestartAppImage: return ok(QProcess::startDetached(arg, {}));
        case UpdateAction::OpenPage: return ok(QDesktopServices::openUrl(QUrl(arg)));
    }
    return LaunchResult::Failed;
}

std::optional<UpdateAction> installAction(PackageFormat format) {
    if (format == PackageFormat::Setup) return UpdateAction::RunInstaller;
    if (format == PackageFormat::Dmg) return UpdateAction::OpenDiskImage;
    return std::nullopt;
}

void setUpdateActionForTest(std::function<bool(UpdateAction, const QString&)> action) { testAction() = std::move(action); }

}  // namespace construe::app
