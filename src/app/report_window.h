// «Report a problem» window (spec/app-ux.md §10.8, S4-BUG-001): modeless; description, e-mail,
// screenshots, the part file checkbox, automatic attachments; sending to the report receiver via Qt Network, a zip for
// manual sending when there is no connection; the crash mode after a crash.
#pragma once

#include "app/app_settings.h"
#include "app/path_list_edit.h"
#include "app/problem_report.h"

#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

namespace construe::app {

class ReportWindow : public QWidget {
    Q_OBJECT
public:
    struct Hooks {
        std::function<QImage()> grabMainWindow;                   // «Window snapshot»
        std::function<std::optional<ReportFile>()> currentDocument;  // the current document as .construe
    };
    ReportWindow(AppSettings& settings, Hooks hooks, bool testMode, QWidget* parent);

    // Manual report (kind = manual); dataDir — the data directory (§10.5), empty — no log.
    void openManual(const QString& dataDir);
    // Report after a crash (kind = crash): the dump and the crashed session's log are attached.
    void openCrash(const QString& dataDir, const CrashMarker& marker);

    bool crashMode() const { return crash_; }
    bool sending() const { return reply_ != nullptr; }
    QString status() const { return status_->text(); }
    // Captcha (§10.8 «Капча»): the token of the shown picture (empty — none) and whether a picture is being requested.
    QString captchaToken() const { return captchaToken_; }
    bool captchaLoading() const { return captchaReply_ != nullptr; }
    // The report as it would be sent now (for the window state and tests).
    ProblemReport currentReport() const;

signals:
    void sent(const QString& id);         // the server accepted the report
    void savedZip(const QString& path);   // the report was saved for manual sending

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void reset();
    void syncButtons();
    void addScreenshot(const QString& name, const QByteArray& data);
    void send();
    void finished();
    void offerZip(const QString& reason);
    void loadCaptcha();  // requests a new picture (the previous request is aborted)
    void captchaFinished();
    void done();  // the report is handled: the crash marker is cleared, the window closes

    AppSettings& settings_;
    Hooks hooks_;
    bool testMode_;
    bool crash_ = false;
    QString dataDir_;
    std::optional<CrashMarker> marker_;
    std::vector<ReportFile> shots_;
    int shotCounter_ = 0;
    QNetworkAccessManager net_;
    QPointer<QNetworkReply> reply_;
    QLabel* intro_ = nullptr;
    QPlainTextEdit* description_ = nullptr;
    QLineEdit* email_ = nullptr;
    PathListEdit* shotList_ = nullptr;  // screenshots with «Add file…» / «Remove»
    QPushButton* shot_ = nullptr;
    QCheckBox* attachDocument_ = nullptr;
    QLabel* auto_ = nullptr;
    QPointer<QNetworkReply> captchaReply_;
    QString captchaToken_;
    QTimer captchaTimer_;  // the picture's lifetime
    QLabel* captchaImage_ = nullptr;
    QLineEdit* captchaAnswer_ = nullptr;
    QPushButton* captchaRefresh_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton *send_ = nullptr, *cancel_ = nullptr;
};

}  // namespace construe::app
