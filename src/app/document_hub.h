// Document events shared by all program windows (app-ux §8а): saving a file in any window is a reason to check
// the other windows for changes in linked files.
#pragma once

#include <QObject>
#include <QString>

namespace construe::app {

class DocumentHub : public QObject {
    Q_OBJECT
public:
    static DocumentHub& instance() {
        static DocumentHub h;
        return h;
    }

signals:
    void saved(const QString& path);
};

}  // namespace construe::app
