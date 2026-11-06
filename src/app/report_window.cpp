#include "app/report_window.h"

#include "app/messages.h"
#include "app/session_log.h"

#include <QBuffer>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

namespace {

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

ReportWindow::ReportWindow(AppSettings& settings, Hooks hooks, bool testMode, QWidget* parent)
    : QWidget(parent, Qt::Window), settings_(settings), hooks_(std::move(hooks)), testMode_(testMode) {
    setObjectName("reportWindow");
    setWindowTitle(text("ui.report.title"));
    auto* col = new QVBoxLayout(this);
    intro_ = new QLabel;
    intro_->setObjectName("reportIntro");
    intro_->setWordWrap(true);
    col->addWidget(intro_);
    auto* form = new QFormLayout;
    description_ = new QPlainTextEdit;
    description_->setObjectName("reportDescription");
    form->addRow(txt("ui.report.problemDescription"), description_);
    email_ = new QLineEdit;
    email_->setObjectName("reportEmail");
    email_->setPlaceholderText("name@example.com");
    form->addRow(txt("ui.report.eMailForReply"), email_);
    col->addLayout(form);
    // Screenshots: a list and the add/remove buttons.
    col->addWidget(new QLabel(txt("ui.report.screenshots")));
    shotList_ = new PathListEdit({"reportShots", "reportAddShot", "reportRemoveShot"}, txt("ui.report.addFile"), txt("ui.report.remove"));
    shotList_->list()->setMaximumHeight(90);
    shot_ = new QPushButton(txt("ui.report.windowSnapshot"));
    shot_->setObjectName("reportShot");
    shotList_->insertButton(shot_);
    col->addWidget(shotList_);
    attachDocument_ = new QCheckBox(txt("ui.report.attachThePartFileTo"));
    attachDocument_->setObjectName("reportAttachDocument");
    col->addWidget(attachDocument_);
    auto_ = new QLabel;
    auto_->setObjectName("reportAuto");
    auto_->setWordWrap(true);
    auto_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(auto_);
    // Captcha (§10.8 «Капча», FX14-SEC-001): the picture, the answer and «Another picture».
    auto* captchaRow = new QHBoxLayout;
    captchaRow->addWidget(new QLabel(txt("ui.report.captchaLabel")));
    captchaImage_ = new QLabel;
    captchaImage_->setObjectName("reportCaptchaImage");
    captchaImage_->setMinimumSize(200, 70);
    captchaImage_->setWordWrap(true);
    captchaRow->addWidget(captchaImage_);
    captchaAnswer_ = new QLineEdit;
    captchaAnswer_->setObjectName("reportCaptchaAnswer");
    captchaAnswer_->setMaxLength(10);
    captchaAnswer_->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9 ]*"), captchaAnswer_));
    captchaAnswer_->setMaximumWidth(120);
    captchaRow->addWidget(captchaAnswer_);
    captchaRefresh_ = new QPushButton(txt("ui.report.captchaRefresh"));
    captchaRefresh_->setObjectName("reportCaptchaRefresh");
    captchaRow->addWidget(captchaRefresh_);
    captchaRow->addStretch(1);
    col->addLayout(captchaRow);
    captchaTimer_.setSingleShot(true);
    status_ = new QLabel;
    status_->setObjectName("reportStatus");
    status_->setWordWrap(true);
    col->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    send_ = new QPushButton(txt("ui.report.send"));
    send_->setObjectName("reportSend");
    cancel_ = new QPushButton(txt("ui.common.cancel"));
    cancel_->setObjectName("reportCancel");
    buttons->addWidget(send_);
    buttons->addWidget(cancel_);
    col->addLayout(buttons);
    resize(560, 600);

    connect(description_, &QPlainTextEdit::textChanged, this, &ReportWindow::syncButtons);
    connect(email_, &QLineEdit::textChanged, this, &ReportWindow::syncButtons);
    connect(captchaAnswer_, &QLineEdit::textChanged, this, &ReportWindow::syncButtons);
    connect(captchaRefresh_, &QPushButton::clicked, this, &ReportWindow::loadCaptcha);
    // The picture is outdated: a new one if no answer is typed yet (a typed one — the server says captcha_expired).
    connect(&captchaTimer_, &QTimer::timeout, this, [this] {
        if (captchaAnswer_->text().trimmed().isEmpty() && !reply_) loadCaptcha();
    });
    connect(shotList_->list(), &QListWidget::currentRowChanged, this, &ReportWindow::syncButtons);
    connect(shot_, &QPushButton::clicked, this, [this] {
        const QImage img = hooks_.grabMainWindow ? hooks_.grabMainWindow() : QImage();
        if (img.isNull()) return;
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        addScreenshot("screenshot-" + QString::number(++shotCounter_) + ".png", png);
    });
    connect(shotList_, &PathListEdit::addRequested, this, [this] {
        const QFileDialog::Options opt = testMode_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
        const QString path = QFileDialog::getOpenFileName(this, txt("ui.report.addScreenshot"), {}, txt("ui.report.imagesPngJpgJpeg"), nullptr, opt);
        if (path.isEmpty()) return;
        const QByteArray data = readFile(path);
        if (!data.isEmpty()) addScreenshot(QFileInfo(path).fileName(), data);
    });
    connect(shotList_, &PathListEdit::removeRequested, this, [this](int row) {
        if (row >= int(shots_.size())) return;
        shots_.erase(shots_.begin() + row);
        shotList_->removeAt(row);
        syncButtons();
    });
    connect(send_, &QPushButton::clicked, this, &ReportWindow::send);
    connect(cancel_, &QPushButton::clicked, this, &QWidget::close);
}

void ReportWindow::reset() {
    if (reply_) reply_->abort();
    description_->clear();
    shots_.clear();
    shotList_->setPaths({});
    shotCounter_ = 0;
    status_->clear();
    if (email_->text().trimmed().isEmpty()) email_->setText(settings_.value("report/email").toString());
    loadCaptcha();
}

void ReportWindow::loadCaptcha() {
    if (captchaReply_) {
        QNetworkReply* old = captchaReply_;
        captchaReply_ = nullptr;
        old->abort();
    }
    captchaTimer_.stop();
    captchaToken_.clear();
    captchaAnswer_->clear();
    captchaImage_->setPixmap(QPixmap());
    // Tests never contact the real server: without a test receiver there is no picture.
    if (testMode_ && qEnvironmentVariableIsEmpty(REPORT_URL_ENV)) {
        captchaImage_->clear();
        syncButtons();
        return;
    }
    captchaImage_->setText(text("ui.report.captchaLoading"));
    QNetworkRequest req{QUrl(captchaUrl())};
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("construe/") + reportVersion().toUtf8());
    req.setTransferTimeout(REPORT_TIMEOUT_MS);
    captchaReply_ = net_.get(req);
    connect(captchaReply_, &QNetworkReply::finished, this, &ReportWindow::captchaFinished);
    syncButtons();
}

void ReportWindow::captchaFinished() {
    auto* rep = qobject_cast<QNetworkReply*>(sender());
    if (!rep) return;
    rep->deleteLater();
    if (rep != captchaReply_) return;  // aborted and replaced by a newer request
    captchaReply_ = nullptr;
    const int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString netError = code == 0 && rep->error() != QNetworkReply::NoError ? rep->errorString() : QString();
    const CaptchaReply c = parseCaptchaReply(code, rep->readAll(), netError);
    QPixmap pix;
    if (c.kind == CaptchaReply::Kind::Success && pix.loadFromData(c.png, "PNG")) {
        captchaToken_ = c.token;
        captchaImage_->setPixmap(pix);
        captchaTimer_.start(std::max(1, c.expiresInSec - 5) * 1000);
    } else {
        captchaImage_->setText(c.kind == CaptchaReply::Kind::Success ? text("ui.report.captchaFailed", {{"detail", "PNG"}})
                                                                     : captchaFailureText(c));
    }
    syncButtons();
}

void ReportWindow::openManual(const QString& dataDir) {
    if (!isVisible() || crash_) {
        crash_ = false;
        marker_.reset();
        dataDir_ = dataDir;
        reset();
        attachDocument_->setEnabled(true);
        attachDocument_->setChecked(true);
    }
    setWindowTitle(text("ui.report.title"));
    intro_->setText(text("ui.report.intro"));
    cancel_->setText(txt("ui.common.cancel"));
    auto_->setText(text("ui.report.auto", {{"dump", ""}, {"version", reportVersion()}, {"os", reportOs()}}));
    syncButtons();
    show();
    raise();
    activateWindow();
}

void ReportWindow::openCrash(const QString& dataDir, const CrashMarker& marker) {
    crash_ = true;
    marker_ = marker;
    dataDir_ = dataDir;
    reset();
    // The part file — the crashed process's copy of unsaved work (§10.5), if any.
    const bool copy = QFileInfo::exists(dataDir + "/recovery-" + QString::number(marker.pid) + ".construe");
    attachDocument_->setEnabled(copy);
    attachDocument_->setChecked(copy);
    setWindowTitle(text("ui.report.crashTitle"));
    intro_->setText(text("ui.report.crashIntro"));
    cancel_->setText(txt("ui.report.donTSend"));
    auto_->setText(text("ui.report.auto", {{"dump", marker.dump ? text("ui.report.autoDump") : QString()},
                                           {"version", reportVersion()},
                                           {"os", reportOs()}}));
    syncButtons();
    show();
    raise();
    activateWindow();
}

void ReportWindow::addScreenshot(const QString& name, const QByteArray& data) {
    if (int(shots_.size()) >= REPORT_MAX_FILES) return;
    QStringList names;
    for (const auto& s : shots_) names << s.name;
    const QString n = uniqueName(name, names);
    shots_.push_back({n, data});
    shotList_->append(n);
    syncButtons();
}

void ReportWindow::syncButtons() {
    const bool busy = reply_ != nullptr;
    const bool full = int(shots_.size()) >= REPORT_MAX_FILES;
    shot_->setEnabled(!busy && !full);
    shotList_->setAddEnabled(!busy && !full);
    shotList_->setRemoveAllowed(!busy);
    captchaRefresh_->setEnabled(!busy);
    const bool answered = captchaToken_.isEmpty() || !captchaAnswer_->text().trimmed().isEmpty();
    send_->setEnabled(!busy && answered && canSendReport(description_->toPlainText(), email_->text()));
}

ProblemReport ReportWindow::currentReport() const {
    ProblemReport r;
    r.description = description_->toPlainText().trimmed();
    r.email = email_->text().trimmed();
    r.version = reportVersion();
    r.os = reportOs();
    r.kind = crash_ ? "crash" : "manual";
    r.screenshots = shots_;
    QStringList names;
    auto attach = [&](const QString& name, const QByteArray& data) {
        const QString n = uniqueName(name, names);
        names << n;
        r.attachments.push_back({n, data});
    };
    if (!dataDir_.isEmpty()) {
        const QString log = crash_ ? previousLogPath(dataDir_) : logPath(dataDir_);
        if (QFileInfo::exists(log)) attach("construe.log", readFile(log));
        // The crashed session's action trace — as saved on disk (FX14-UI-003).
        if (crash_ && QFileInfo::exists(previousActionsLogPath(dataDir_))) attach("actions.log", readFile(previousActionsLogPath(dataDir_)));
    }
    // A manual report: the trace of this session from memory (the last ACTION_TRACE_WINDOW_MS).
    if (!crash_) {
        const QByteArray trace = actionTrace().text();
        if (!trace.isEmpty()) attach("actions.log", trace);
    }
    if (!dataDir_.isEmpty()) {
        if (crash_ && marker_ && marker_->dump && QFileInfo::exists(crashDumpPath(dataDir_))) attach("crash.dmp", readFile(crashDumpPath(dataDir_)));
    }
    if (attachDocument_->isEnabled() && attachDocument_->isChecked()) {
        if (crash_ && marker_) {
            const QString copy = dataDir_ + "/recovery-" + QString::number(marker_->pid) + ".construe";
            if (QFileInfo::exists(copy)) attach("document.construe", readFile(copy));
        } else if (hooks_.currentDocument) {
            if (auto doc = hooks_.currentDocument()) attach(doc->name, doc->data);
        }
    }
    return r;
}

void ReportWindow::send() {
    if (reply_ || !send_->isEnabled()) return;
    const ProblemReport r = currentReport();
    const QByteArray boundary = newBoundary();
    const QByteArray body = reportBody(r, boundary);
    if (body.size() > REPORT_MAX_BYTES) {
        offerZip(text("ui.report.tooLarge"));
        return;
    }
    QNetworkRequest req{QUrl(reportUrl())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, reportContentType(boundary));
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("construe/") + reportVersion().toUtf8());
    req.setTransferTimeout(REPORT_TIMEOUT_MS);
    // The captcha goes in headers: the server checks it before reading the body (§10.8 «Капча»).
    if (!captchaToken_.isEmpty()) {
        req.setRawHeader(CAPTCHA_HEADER, captchaToken_.toLatin1());
        req.setRawHeader(CAPTCHA_ANSWER_HEADER, captchaAnswer_->text().trimmed().toLatin1());
    }
    captchaTimer_.stop();
    status_->setText(text("ui.report.sending"));
    reply_ = net_.post(req, body);
    connect(reply_, &QNetworkReply::finished, this, &ReportWindow::finished);
    syncButtons();
}

void ReportWindow::finished() {
    QNetworkReply* rep = reply_;
    reply_ = nullptr;
    if (!rep) return;
    rep->deleteLater();
    if (rep->error() == QNetworkReply::OperationCanceledError) return;  // aborted by closing the window
    const int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString netError = code == 0 && rep->error() != QNetworkReply::NoError ? rep->errorString() : QString();
    const ReportReply r = parseReportReply(code, rep->readAll(), netError);
    syncButtons();
    switch (r.kind) {
        case ReportReply::Kind::Success: {
            settings_.set("report/email", email_->text().trimmed());
            const QString id = r.id;
            status_->setText(text("ui.report.sent", {{"id", id}}));
            done();
            description_->clear();
            shots_.clear();
            shotList_->setPaths({});
            shotCounter_ = 0;
            emit sent(id);
            break;
        }
        case ReportReply::Kind::Refused:
            status_->setText(text("ui.report.serverError", {{"message", reportErrorText(r)}}));
            loadCaptcha();  // the server used the token up on any check
            break;
        case ReportReply::Kind::Offline: offerZip(text("ui.report.offline", {{"detail", r.detail}})); break;
    }
}

void ReportWindow::offerZip(const QString& reason) {
    status_->setText(reason);
    const QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const QDateTime now = QDateTime::currentDateTime();
    QString path = QFileDialog::getSaveFileName(this, txt("ui.report.saveReport"), "construe-report-" + now.toString("yyyyMMdd-HHmmss") + ".zip",
                                                txt("ui.report.zipArchiveZip"), nullptr, opt);
    if (path.isEmpty()) return;
    if (!path.endsWith(".zip", Qt::CaseInsensitive)) path += ".zip";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(reportZip(currentReport(), now)) < 0) {
        status_->setText(text("ui.report.saveFailed", {{"path", path}}));
        return;
    }
    f.close();
    status_->setText(text("ui.report.savedZip", {{"path", path}}));
    done();
    emit savedZip(path);
}

void ReportWindow::done() {
    if (crash_) clearCrashReport(dataDir_);
    marker_.reset();
    hide();
    crash_ = false;
}

void ReportWindow::closeEvent(QCloseEvent* e) {
    // Closing a crash report («Don't send») declines it: the crash is no longer offered.
    if (crash_) clearCrashReport(dataDir_);
    crash_ = false;
    marker_.reset();
    if (reply_) reply_->abort();
    if (captchaReply_) {
        QNetworkReply* pending = captchaReply_;
        captchaReply_ = nullptr;
        pending->abort();
    }
    captchaTimer_.stop();
    QWidget::closeEvent(e);
}

}  // namespace construe::app
