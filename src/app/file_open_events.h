// Documents the system asks the running program to open (spec/app-ux.md §10.1, S8-PKG-003): on macOS a double click on
// a .construe file in Finder or a drop on the Dock icon arrives as QFileOpenEvent, not as a command-line argument.
#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace construe::app {

// Installed on the application object: each QEvent::FileOpen with a local file is handed to `open` and consumed;
// other events pass. Removed from the application when destroyed (a child of the main window).
class FileOpenEvents : public QObject {
public:
    FileOpenEvents(std::function<void(const QString&)> open, QObject* parent);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::function<void(const QString&)> open_;
};

}  // namespace construe::app
