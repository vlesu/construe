// Documents the system asks the running program to open (app/file_open_events.h).
#include "app/file_open_events.h"

#include <QCoreApplication>
#include <QFileOpenEvent>

namespace construe::app {

FileOpenEvents::FileOpenEvents(std::function<void(const QString&)> open, QObject* parent)
    : QObject(parent), open_(std::move(open)) {
    if (auto* app = QCoreApplication::instance()) app->installEventFilter(this);
}

bool FileOpenEvents::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FileOpen) {
        const QString path = static_cast<QFileOpenEvent*>(event)->file();  // local file; empty for a URL
        if (!path.isEmpty() && open_) {
            open_(path);
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace construe::app
