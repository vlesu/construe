// Update window (spec/app-ux.md §10.10, spec/update.md §4–§5, REL-CT-002, REL-CT-003): modeless; checks the
// latest-version description of this build's system via Qt Network (only the description), offers a newer version
// with its notes and updates by the package format: downloads the package with progress and cancellation, verifies its
// size and SHA-256 and hands an installer or disk image to the main window, replaces an AppImage in place (or saves it
// to «Downloads»), shows the Flatpak command or the project page for builds without a package.
#pragma once

#include "app/update_check.h"

#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include <optional>

namespace construe::app {

class UpdateWindow : public QWidget {
    Q_OBJECT
public:
    enum class Result { Newer, Latest, NotPublished, Failed };
    Q_ENUM(Result)
    explicit UpdateWindow(QWidget* parent);

    // Starts a check (update.md §4). false — a check or download is already in progress: a manual request then joins
    // the running check (its result is reported as manual) or raises the window during a download. A manual check
    // shows the window at once («Checking for updates…») and its result there (app-ux §10.10, FX8-UI-004).
    bool check(bool manual);
    bool checking() const { return checkReply_ != nullptr; }
    bool downloading() const { return downloadReply_ != nullptr; }
    QString status() const { return status_->text(); }
    // The offered version (the last check found a newer one).
    const std::optional<UpdateInfo>& offered() const { return offered_; }
    // A status line after the main window did not install (cancelled or failed launch): the buttons return.
    void installNotStarted(const QString& message);

    QString downloadDir() const { return downloadDir_; }
    void setDownloadDirForTest(const QString& dir) { downloadDir_ = dir.isEmpty() ? defaultUpdateDownloadDir() : dir; }
    // The «Downloads» folder of the AppImage fallback (update.md §5 «AppImage»).
    void setSaveDirForTest(const QString& dir) { saveDir_ = dir; }

signals:
    // A check finished; message — the notification text for a manual check (empty for Newer).
    void checkFinished(bool manual, construe::app::UpdateWindow::Result result, const QString& message);
    // The installer or disk image is downloaded and verified (update.md §5 «Download» step 4).
    void installReady(const QString& path);
    // «Restart» after the AppImage was replaced in place (update.md §5 «AppImage» step 4).
    void restartRequested(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void offer(const UpdateInfo& info);
    void showChecking();
    // A manual check without an offer: the headline, the installed version, a status line; failed — «Retry».
    void showMessage(const QString& headline, const QString& status, bool failed);
    void checkDone();
    void startDownload();
    void downloadData();
    void downloadDone();
    void syncButtons();
    bool openPart(const QString& dir, const QString& name);

    QNetworkAccessManager net_;
    QPointer<QNetworkReply> checkReply_, downloadReply_;
    bool manual_ = false;
    bool tooLarge_ = false;
    std::optional<UpdateInfo> offered_;
    PackageFormat format_ = PackageFormat::None;  // of the offer
    QString downloadDir_, saveDir_;
    QString partPath_, targetPath_;  // the download and where it goes; for AppImage in place — APPIMAGE
    bool inPlace_ = false;           // AppImage replaced in place (otherwise saved to «Downloads»)
    QString replaced_;               // the replaced AppImage: «Restart» is shown
    QFile part_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 received_ = 0;
    bool cancelled_ = false, overflow_ = false, writeFailed_ = false;
    QLabel *text_ = nullptr, *current_ = nullptr, *how_ = nullptr, *status_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QLineEdit* command_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton *install_ = nullptr, *openPage_ = nullptr, *restart_ = nullptr, *later_ = nullptr, *cancel_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton *retry_ = nullptr, *close_ = nullptr;
    bool message_ = false, failed_ = false;  // the window shows a check result, not an offer
};

}  // namespace construe::app
