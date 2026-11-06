// Updating from the program without widgets (spec/update.md, REL-CT-002, REL-CT-003): program version and build
// number, version ordering, the update system and package format, the latest-version description (latest.json), the
// automatic check interval, the package file name, replacing an AppImage and the external update actions.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <compare>
#include <functional>
#include <optional>

namespace construe::app {

// Constants of spec/update.md §8.
inline constexpr const char* UPDATE_BASE_URL = "https://construe.vlesu.com/updates/";
inline constexpr const char* UPDATE_URL_ENV = "CONSTRUE_UPDATE_URL";
inline constexpr const char* UPDATE_PAGE_URL = "https://construe.vlesu.com/";
inline constexpr const char* FLATPAK_APP_ID = "com.vlesu.construe";
inline constexpr const char* APPIMAGE_ENV = "APPIMAGE";
inline constexpr qint64 UPDATE_CHECK_INTERVAL_S = 7LL * 24 * 3600;
inline constexpr int UPDATE_START_DELAY_MS = 5000;
inline constexpr int UPDATE_CHECK_TIMEOUT_MS = 15000;
inline constexpr qint64 UPDATE_INFO_MAX_BYTES = 65536;
inline constexpr int UPDATE_DOWNLOAD_STALL_MS = 60000;
inline constexpr qint64 UPDATE_MAX_INSTALLER_BYTES = 1024LL * 1024 * 1024;
inline constexpr int UPDATE_NOTES_MAX_CHARS = 4000;
inline constexpr int UPDATE_FORMAT_VERSION = 1;
// Settings key of the last check start (app-ux §10.6).
inline constexpr const char* UPDATE_LAST_CHECK_KEY = "update/lastCheck";

// Program version (§2): x.y.z, the pre-release label (empty — a release; "alpha", "beta.2", …) and the build number
// (0 — unknown). Ordered by x.y.z, then by the label as SemVer 2.0.0 §11 orders pre-releases (a version with a label
// precedes the same x.y.z without one), then by the build number. The build number is the UTC commit time
// YYYYMMDDhhmm (FX14-PKG-001) — 64-bit; old numbers (commit counts) are smaller than any of them.
inline constexpr qint64 BUILD_NUMBER_MAX = 999999999999LL;
inline constexpr int PRERELEASE_MAX_CHARS = 32;
struct AppVersion {
    int major = 0, minor = 0, patch = 0;
    qint64 build = 0;
    QString prerelease;  // last, so {x, y, z, build} still initialises a release
    bool operator==(const AppVersion&) const = default;
    std::strong_ordering operator<=>(const AppVersion& other) const;
};

// SemVer pre-release precedence of two labels (§2): an empty label (a release) is the greatest; identifiers are compared
// left to right — numeric ones as numbers and below alphanumeric ones, alphanumeric ones by ASCII; a shorter label
// whose identifiers all equal the start of a longer one precedes it.
std::strong_ordering comparePrerelease(const QString& a, const QString& b);
// A valid pre-release label (§2): dot-separated identifiers of [0-9A-Za-z], the first starting with a letter, numeric
// ones without leading zeros, at most PRERELEASE_MAX_CHARS characters. Empty is not a label.
bool validPrerelease(const QString& label);

// The version of this build: CONSTRUE_VERSION, CONSTRUE_VERSION_PRERELEASE and the generated build number.
AppVersion currentVersion();
// Short commit hash of this build (empty — unknown).
QString currentCommit();
// "x.y.z" or "x.y.z-<label>" → version with build 0; nullopt — not three decimal numbers without leading zeros (each
// ≤ 999 999 999) or an invalid label.
std::optional<AppVersion> parseVersion(const QString& text);
// "x.y.z" or "x.y.z-<label>" (§2): the version without the build number.
QString versionString(const AppVersion& v);
// A date build number (§2): 12 digits YYYYMMDDhhmm with valid fields (year ≥ 2000); its UTC time — invalid otherwise.
QDateTime buildDateTime(qint64 build);
// The build tag for file names (§2): a date number — "YYYYMMDD-hhmm", another — the number itself.
QString buildTag(qint64 build);
// For people (§2): ui.update.versionBuild with {version} = versionString — a date number as "YYYY-MM-DD hh:mm UTC",
// another as is; build 0 — just versionString.
QString versionText(const AppVersion& v);
// Package version (§2): "<versionString>-<build tag>-<commit>", without a commit — "<versionString>-<build tag>".
QString packageVersion(const AppVersion& v, const QString& commit);
QString packageVersion();  // of this build

// Update system and package format of this build (§2а): compiled in (CONSTRUE_UPDATE_OS, CONSTRUE_PACKAGE_FORMAT),
// replaced by a test. Zip is a run-time format only: a setup build running without installation (S8-PKG-001).
enum class PackageFormat { Setup, AppImage, Flatpak, Dmg, None, Zip };
QString updateOs();                // "windows", "linux" or "macos"
PackageFormat packageFormat();
// Running without installation (app-ux §10.5а): a setup build then works in the zip format (§2а). Set at start-up.
void setPortableMode(bool portable);
// "setup", "appimage", "flatpak", "dmg", "none", "zip" ↔ the format; nullopt — unknown name.
std::optional<PackageFormat> parsePackageFormat(const QString& name);
QString packageFormatName(PackageFormat f);
// The allowed combinations of §2а: setup and zip — windows, appimage and flatpak — linux, dmg — macos, none — any.
bool formatFitsOs(PackageFormat f, const QString& os);
// A test target (empty os — back to the compiled-in one).
void setUpdateTargetForTest(const QString& os, PackageFormat format);
// Whether the format downloads a package on «Download and install» (setup, appimage, dmg, zip — when the description
// has a zip, §3 rule 9).
bool formatDownloads(PackageFormat f);
// The command of the Flatpak hint (§5): "flatpak update <FLATPAK_APP_ID>".
QString flatpakUpdateCommand();

// The updates folder (CONSTRUE_UPDATE_URL if set, otherwise UPDATE_BASE_URL; a trailing "/" is added) and the
// description address of a folder: <base><folder>/latest.json (§3).
QString updateBaseUrl();
QString updateUrl(const QString& folder);
QString updateUrl();  // of this build: updateFolder(updateOs(), buildArch())
// The description folder of a system (§3): the system itself; macOS — "macos-<arch>" (a DMG per architecture).
QString updateFolder(const QString& os, const QString& arch);
// The processor architecture the program is compiled for: "arm64", "x86_64" or QSysInfo's name of another one.
QString buildArch();
bool updateUrlOverridden();

// latest.json (§3).
struct PackageRef {
    QUrl url;            // resolved against the description address
    QByteArray sha256;   // 64 lowercase hex digits
    qint64 size = 0;
};
struct UpdateInfo {
    AppVersion version;
    QUrl url;            // resolved against the description address
    QByteArray sha256;   // 64 lowercase hex digits
    qint64 size = 0;
    QString notes;
    std::optional<PackageRef> zip;  // the Windows zip without installation (§3 rule 9); nullopt — absent or invalid
};
// nullopt — a format error (any rule of §3 broken).
std::optional<UpdateInfo> parseUpdateInfo(const QByteArray& body, const QUrl& base);
// The offer of a format (§5): for zip — the description with url, sha256 and size of its zip; nullopt — the zip format
// and no zip in the description. Other formats — the description itself.
std::optional<UpdateInfo> offerFor(const UpdateInfo& info, PackageFormat format);
// Notes for display: trimmed to UPDATE_NOTES_MAX_CHARS characters with "…".
QString displayNotes(const QString& notes);

// The automatic check is due (§4): last is empty, not ISO 8601, later than now, or at least the interval ago.
bool updateCheckDue(const QString& lastCheck, const QDateTime& now);
// The value written to update/lastCheck (ISO 8601, UTC, seconds).
QString updateCheckStamp(const QDateTime& now);

// Package file name (§5 «Download» step 1): the last segment of url with the format's extension, otherwise by version.
QString packageFileName(const UpdateInfo& info, PackageFormat format);
// Download folder of setup and dmg: <temp>/construe-update (zip — the «Downloads» folder, defaultUpdateSaveDir).
QString defaultUpdateDownloadDir();
// The «Downloads» folder of the AppImage fallback (§5 «AppImage» step 1): DownloadLocation, empty — home.
QString defaultUpdateSaveDir();
// The running AppImage (APPIMAGE) when it can be replaced in place: set, an existing file in a writable folder;
// otherwise empty.
QString replaceableAppImage();
// Sets rwxr-xr-x on a downloaded AppImage.
bool makeExecutable(const QString& path);
// Makes the verified file executable and renames it over target atomically (POSIX rename). false — failed.
bool replaceAppImage(const QString& downloaded, const QString& target);

// External update actions (§6). A function set by a test replaces all of them on every system.
enum class UpdateAction { RunInstaller, OpenDiskImage, RestartAppImage, OpenPage };
enum class LaunchResult { Launched, Failed, Unsupported };
LaunchResult runUpdateAction(UpdateAction action, const QString& arg);
// The action of «Install» for a downloaded package (§5): setup — RunInstaller, dmg — OpenDiskImage; nullopt — the
// format has no such step (appimage, flatpak, none).
std::optional<UpdateAction> installAction(PackageFormat format);
void setUpdateActionForTest(std::function<bool(UpdateAction, const QString&)> action);

}  // namespace construe::app
