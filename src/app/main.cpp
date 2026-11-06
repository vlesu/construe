// construe application entry point (spec/app-ux.md §12.1: --test-mode flag).
#include "app/crash_handler.h"
#include "app/foreground.h"
#include "app/gl_setup.h"
#include "app/icons.h"
#include "app/main_window.h"
#include "app/portable.h"
#include "app/session_log.h"
#include "app/update_check.h"

#include <QApplication>
#include <QTimer>

int main(int argc, char** argv) {
    bool testMode = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--test-mode") testMode = true;
    construe::app::configureBeforeApplication(testMode);
    QApplication app(argc, argv);
    app.setApplicationName("construe");
    app.setWindowIcon(construe::app::appIcon());  // all program windows and the taskbar (S4-APP-001)
    // Program log file and crash handler (app-ux §10.8, S4-BUG-001); not in test mode — tests set their own data directory.
    // Without installation (the marker next to the program, §10.5а) the data, the settings and the update format change.
    if (!testMode) {
        const construe::app::StorageLayout layout = construe::app::storageLayout();
        if (layout.portable) {
            QCoreApplication::setAttribute(Qt::AA_DisableShaderDiskCache);  // Qt would write it to the profile cache
            construe::app::setPortableMode(true);
        }
        construe::app::installLogFile(layout.dataDir);
        construe::app::installCrashHandler(layout.dataDir);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
            construe::app::logSession(construe::app::LogLevel::Info, "session end");  // i18n-skip: log
            construe::app::closeSessionLog();
        });
    }
    construe::app::MainWindow w(testMode);
    // The start record of the session log (FX14-UI-003) — after the settings (the interface language) are applied;
    // OpenGL — from the view when it starts.
    if (!testMode) construe::app::logSessionStart(construe::app::storageLayout().dataDir);
    // The window comes up in front and active (app-ux §2.1, FX7-UI-001): once now and once more when the event loop runs —
    // the system may hand the foreground over only after the first window is mapped.
    construe::app::bringToFront(&w);
    QTimer::singleShot(0, &w, [&w] { construe::app::bringToFront(&w); });
    w.openFromCommandLine(QCoreApplication::arguments());  // «construe <file>», double click on a .construe file
    return app.exec();
}
