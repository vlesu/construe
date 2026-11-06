#include "app/update_window.h"

#include "app/messages.h"

#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVBoxLayout>

namespace construe::app {

namespace {

QNetworkRequest updateRequest(const QUrl& url, int timeoutMs) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("construe/") + packageVersion().toUtf8());
    req.setRawHeader("Cache-Control", "no-cache");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(timeoutMs);
    return req;
}

// Failure detail (update.md §4): the network error text without an HTTP reply, otherwise "HTTP <code>".
QString failureDetail(QNetworkReply* rep) {
    const int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code > 0) return "HTTP " + QString::number(code);
    return rep->errorString();
}

bool httpOk(QNetworkReply* rep) {
    const int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return rep->error() == QNetworkReply::NoError && code >= 200 && code < 300;
}

}  // namespace

UpdateWindow::UpdateWindow(QWidget* parent) : QWidget(parent, Qt::Window), downloadDir_(defaultUpdateDownloadDir()) {
    setObjectName("updateWindow");
    setWindowTitle(text("ui.update.title"));
    auto* col = new QVBoxLayout(this);
    text_ = new QLabel;
    text_->setObjectName("updateText");
    text_->setWordWrap(true);
    QFont bold = text_->font();
    bold.setBold(true);
    text_->setFont(bold);
    col->addWidget(text_);
    current_ = new QLabel;
    current_->setObjectName("updateCurrent");
    current_->setWordWrap(true);
    col->addWidget(current_);
    notes_ = new QPlainTextEdit;
    notes_->setObjectName("updateNotes");
    notes_->setReadOnly(true);
    col->addWidget(notes_, 1);
    how_ = new QLabel;
    how_->setObjectName("updateHow");
    how_->setWordWrap(true);
    how_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(how_);
    auto* commandRow = new QHBoxLayout;
    command_ = new QLineEdit;
    command_->setObjectName("updateCommand");
    command_->setReadOnly(true);
    copy_ = new QPushButton(txt("ui.common.copy"));
    copy_->setObjectName("updateCopy");
    commandRow->addWidget(command_, 1);
    commandRow->addWidget(copy_);
    col->addLayout(commandRow);
    progress_ = new QProgressBar;
    progress_->setObjectName("updateProgress");
    col->addWidget(progress_);
    status_ = new QLabel;
    status_->setObjectName("updateStatus");
    status_->setWordWrap(true);
    col->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    install_ = new QPushButton(txt("ui.update.downloadAndInstall"));
    install_->setObjectName("updateInstall");
    openPage_ = new QPushButton(txt("ui.update.openPage"));
    openPage_->setObjectName("updateOpenPage");
    restart_ = new QPushButton(txt("ui.update.restart"));
    restart_->setObjectName("updateRestart");
    later_ = new QPushButton(txt("ui.update.later"));
    later_->setObjectName("updateLater");
    cancel_ = new QPushButton(txt("ui.common.cancel"));
    cancel_->setObjectName("updateCancel");
    // A manual check without an offer (FX8-UI-004): «Retry» after an error, «Close».
    retry_ = new QPushButton(txt("ui.update.retry"));
    retry_->setObjectName("updateRetry");
    close_ = new QPushButton(txt("ui.common.close"));
    close_->setObjectName("updateClose");
    buttons->addWidget(install_);
    buttons->addWidget(openPage_);
    buttons->addWidget(restart_);
    buttons->addWidget(later_);
    buttons->addWidget(cancel_);
    buttons->addWidget(retry_);
    buttons->addWidget(close_);
    col->addLayout(buttons);
    resize(480, 320);

    connect(install_, &QPushButton::clicked, this, &UpdateWindow::startDownload);
    connect(later_, &QPushButton::clicked, this, &QWidget::close);
    connect(close_, &QPushButton::clicked, this, &QWidget::close);
    connect(retry_, &QPushButton::clicked, this, [this] { check(true); });
    connect(copy_, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(command_->text());
        status_->setText(text("ui.update.copied"));
    });
    connect(openPage_, &QPushButton::clicked, this, [this] {
        const QString page = QString::fromLatin1(UPDATE_PAGE_URL);
        if (runUpdateAction(UpdateAction::OpenPage, page) != LaunchResult::Launched)
            status_->setText(text("ui.update.pageFailed", {{"url", page}}));
    });
    connect(restart_, &QPushButton::clicked, this, [this] {
        if (!replaced_.isEmpty()) emit restartRequested(replaced_);
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (!downloadReply_) return;
        cancelled_ = true;
        downloadReply_->abort();
    });
    syncButtons();
}

bool UpdateWindow::check(bool manual) {
    if (downloadReply_) {
        show();
        raise();
        activateWindow();
        return false;
    }
    if (manual) showChecking();  // the window answers at once (FX8-UI-004)
    if (checkReply_) {
        manual_ = manual_ || manual;
        return false;
    }
    manual_ = manual;
    tooLarge_ = false;
    const QUrl base(updateUrl());
    checkReply_ = net_.get(updateRequest(base, UPDATE_CHECK_TIMEOUT_MS));
    // A description larger than UPDATE_INFO_MAX_BYTES is not read to the end (update.md §4 step 3).
    connect(checkReply_, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        if (got > UPDATE_INFO_MAX_BYTES && checkReply_) {
            tooLarge_ = true;
            checkReply_->abort();
        }
    });
    connect(checkReply_, &QNetworkReply::finished, this, &UpdateWindow::checkDone);
    return true;
}

void UpdateWindow::checkDone() {
    QNetworkReply* rep = checkReply_;
    checkReply_ = nullptr;
    if (!rep) return;
    rep->deleteLater();
    const bool manual = manual_;
    manual_ = false;
    // A manual check shows its result in the window (FX8-UI-004); the automatic one is silent without a newer version.
    auto finish = [&](Result r, const QString& message, const QString& status = QString()) {
        if (manual) showMessage(message, status, r == Result::Failed);
        emit checkFinished(manual, r, message);
    };
    auto fail = [&](const QString& detail) { finish(Result::Failed, text("ui.update.checkFailed", {{"detail", detail}})); };
    if (tooLarge_) return fail(text("ui.update.badFormat"));
    const int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString latest = text("ui.update.latest", {{"version", versionText(currentVersion())}});
    // No description for this system: its package is not released yet (update.md §4) — no updates for it.
    if (code == 404) return finish(Result::NotPublished, latest, text("ui.update.notPublished"));
    // No HTTP reply: the network (update.md §4) — a message about the connection.
    if (code <= 0 && rep->error() != QNetworkReply::NoError)
        return finish(Result::Failed, text("ui.update.networkFailed", {{"detail", rep->errorString()}}));
    if (!httpOk(rep)) return fail(failureDetail(rep));
    const auto info = parseUpdateInfo(rep->readAll(), rep->url());
    if (!info) return fail(text("ui.update.badFormat"));
    if (info->version > currentVersion()) {
        offer(*info);
        emit checkFinished(manual, Result::Newer, QString());
        return;
    }
    finish(Result::Latest, latest);
}

void UpdateWindow::showChecking() {
    showMessage(text("ui.update.checking"), QString(), false);
    retry_->setVisible(false);
    show();
    raise();
    activateWindow();
}

void UpdateWindow::showMessage(const QString& headline, const QString& status, bool failed) {
    message_ = true;
    failed_ = failed;
    offered_.reset();
    format_ = PackageFormat::None;
    replaced_.clear();
    text_->setText(headline);
    current_->setText(text("ui.update.current", {{"version", versionText(currentVersion())}}));
    notes_->clear();
    notes_->setVisible(false);
    how_->clear();
    command_->clear();
    status_->setText(status);
    syncButtons();
}

void UpdateWindow::offer(const UpdateInfo& info) {
    message_ = failed_ = false;
    format_ = packageFormat();
    // Without installation the zip of the description is offered (update.md §5); no zip in it — the project page.
    const auto offered = offerFor(info, format_);
    offered_ = offered ? *offered : info;
    const bool zipMissing = !offered;
    if (zipMissing) format_ = PackageFormat::None;
    replaced_.clear();
    install_->setText(format_ == PackageFormat::Zip ? txt("ui.update.downloadArchive") : txt("ui.update.downloadAndInstall"));
    // Formats without a download explain how to update (update.md §5).
    const QString page = QString::fromLatin1(UPDATE_PAGE_URL);
    if (format_ == PackageFormat::Flatpak) how_->setText(text("ui.update.flatpakHint"));
    else if (zipMissing) how_->setText(text("ui.update.zipHint", {{"url", page}}));
    else if (format_ == PackageFormat::None) how_->setText(text("ui.update.sourceHint", {{"url", page}}));
    else how_->clear();
    command_->setText(format_ == PackageFormat::Flatpak ? flatpakUpdateCommand() : QString());
    text_->setText(text("ui.update.available", {{"version", versionText(info.version)}}));
    current_->setText(text("ui.update.current", {{"version", versionText(currentVersion())}}));
    const QString notes = displayNotes(info.notes);
    notes_->setPlainText(notes);
    notes_->setVisible(!notes.isEmpty());
    status_->clear();
    syncButtons();
    show();
    raise();
    activateWindow();
}

void UpdateWindow::syncButtons() {
    const bool busy = downloadReply_ != nullptr;
    const bool restart = !replaced_.isEmpty();
    const bool offer = !message_;
    install_->setVisible(offer && !busy && !restart && formatDownloads(format_));
    install_->setEnabled(offered_.has_value());
    openPage_->setVisible(offer && !busy && format_ == PackageFormat::None);
    restart_->setVisible(offer && !busy && restart);
    later_->setVisible(offer && !busy);
    cancel_->setVisible(busy);
    retry_->setVisible(message_ && failed_);
    close_->setVisible(message_);
    progress_->setVisible(busy);
    how_->setVisible(!how_->text().isEmpty());
    command_->setVisible(offer && format_ == PackageFormat::Flatpak);
    copy_->setVisible(offer && format_ == PackageFormat::Flatpak);
}

bool UpdateWindow::openPart(const QString& dir, const QString& name) {
    QDir().mkpath(dir);
    partPath_ = dir + "/" + name + ".part";
    part_.setFileName(partPath_);
    return part_.open(QIODevice::WriteOnly | QIODevice::Truncate);
}

void UpdateWindow::startDownload() {
    if (downloadReply_ || !offered_ || !formatDownloads(format_)) return;
    const UpdateInfo info = *offered_;
    const QString name = packageFileName(info, format_);
    bool opened = false;
    inPlace_ = false;
    if (format_ == PackageFormat::AppImage) {
        // Next to the running AppImage when it can be replaced, otherwise in «Downloads» (update.md §5 «AppImage»).
        const QString current = replaceableAppImage();
        if (!current.isEmpty() && openPart(QFileInfo(current).absolutePath(), name)) {
            opened = inPlace_ = true;
            targetPath_ = current;
        } else {
            const QString dir = saveDir_.isEmpty() ? defaultUpdateSaveDir() : saveDir_;
            opened = openPart(dir, name);
            targetPath_ = dir + "/" + name;
        }
    } else if (format_ == PackageFormat::Zip) {
        // The zip goes to «Downloads»: the user unpacks it into a new folder (update.md §5).
        const QString dir = saveDir_.isEmpty() ? defaultUpdateSaveDir() : saveDir_;
        opened = openPart(dir, name);
        targetPath_ = dir + "/" + name;
    } else {
        opened = openPart(downloadDir_, name);
        targetPath_ = downloadDir_ + "/" + name;
    }
    if (!opened) {
        status_->setText(text("ui.update.saveFailed", {{"path", QDir::toNativeSeparators(partPath_)}}));
        return;
    }
    hash_.reset();
    received_ = 0;
    cancelled_ = overflow_ = writeFailed_ = false;
    progress_->setRange(0, int(info.size));
    progress_->setValue(0);
    status_->setText(text("ui.update.downloading"));
    downloadReply_ = net_.get(updateRequest(info.url, UPDATE_DOWNLOAD_STALL_MS));
    connect(downloadReply_, &QNetworkReply::readyRead, this, &UpdateWindow::downloadData);
    connect(downloadReply_, &QNetworkReply::finished, this, &UpdateWindow::downloadDone);
    syncButtons();
}

void UpdateWindow::downloadData() {
    if (!downloadReply_ || !offered_ || !downloadReply_->isOpen()) return;
    const QByteArray chunk = downloadReply_->readAll();
    if (chunk.isEmpty() || overflow_ || writeFailed_) return;
    received_ += chunk.size();
    if (received_ > offered_->size) {  // more than announced: the file is not the described one (update.md §5 step 2)
        overflow_ = true;
        downloadReply_->abort();
        return;
    }
    if (part_.write(chunk) != chunk.size()) {
        writeFailed_ = true;
        downloadReply_->abort();
        return;
    }
    hash_.addData(chunk);
    progress_->setValue(int(received_));
}

void UpdateWindow::downloadDone() {
    downloadData();  // the rest of the data that arrived with finished
    QNetworkReply* rep = downloadReply_;
    downloadReply_ = nullptr;
    if (!rep) return;
    rep->deleteLater();
    part_.close();
    const UpdateInfo info = *offered_;
    QString failure;
    if (cancelled_) failure = text("ui.update.cancelled");
    else if (writeFailed_) failure = text("ui.update.saveFailed", {{"path", QDir::toNativeSeparators(partPath_)}});
    else if (overflow_) failure = text("ui.update.sizeMismatch");
    else if (!httpOk(rep)) failure = text("ui.update.downloadFailed", {{"detail", failureDetail(rep)}});
    else if (received_ != info.size) failure = text("ui.update.sizeMismatch");
    else if (hash_.result().toHex() != info.sha256) failure = text("ui.update.hashMismatch");
    const QString target = targetPath_;
    const QString shown = QDir::toNativeSeparators(target);
    if (failure.isEmpty()) {
        if (format_ == PackageFormat::AppImage && inPlace_) {
            if (!replaceAppImage(partPath_, target)) failure = text("ui.update.saveFailed", {{"path", shown}});
        } else {
            QFile::remove(target);
            if (!QFile::rename(partPath_, target)) failure = text("ui.update.saveFailed", {{"path", shown}});
            else if (format_ == PackageFormat::AppImage && !makeExecutable(target))
                failure = text("ui.update.saveFailed", {{"path", shown}});
        }
    }
    if (!failure.isEmpty()) {
        QFile::remove(partPath_);  // nothing is installed or replaced (update.md §5 step 3)
        syncButtons();
        status_->setText(failure);
        return;
    }
    if (format_ == PackageFormat::AppImage) {
        if (inPlace_) replaced_ = target;
        syncButtons();
        status_->setText(inPlace_ ? text("ui.update.appImageReplaced") : text("ui.update.appImageSaved", {{"path", shown}}));
        return;
    }
    if (format_ == PackageFormat::Zip) {  // nothing to install: the running folder is not replaced (update.md §5 step 4)
        syncButtons();
        status_->setText(text("ui.update.zipSaved", {{"path", shown}}));
        return;
    }
    syncButtons();
    status_->clear();
    emit installReady(target);
}

void UpdateWindow::installNotStarted(const QString& message) {
    status_->setText(message);
    syncButtons();
    show();
    raise();
}

void UpdateWindow::closeEvent(QCloseEvent* e) {
    // Closing the window during a download cancels it (app-ux §10.10).
    if (downloadReply_) {
        cancelled_ = true;
        downloadReply_->abort();
    }
    QWidget::closeEvent(e);
}

}  // namespace construe::app
