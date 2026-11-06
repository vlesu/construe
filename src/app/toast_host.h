// Notifications (app-ux §9.1): frames with text, action buttons and a close cross in the view's bottom-right corner;
// informational ones close after TOAST_INFO_MS, errors — manually; at most TOAST_MAX at once.
#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace construe::app {

// Notification action button: closes the notification and performs the action.
struct ToastAction {
    QString label;
    std::function<void()> run;
};

// What the application looks like for the conditions of messages (S7F-UI-003).
struct ToastState {
    bool sketchOpen = false;
    std::string active;      // active element ID
    uint64_t revision = 0;   // document revision
    int mode = 0;            // MainWindow mode
    bool hasPath = false;    // the document has a file
};

// When a message lost its sense (S7F-UI-003): any of the set conditions against the state at posting.
enum ToastLapse : unsigned {
    LAPSE_NONE = 0,
    LAPSE_SKETCH_CLOSED = 1,   // no sketch is open
    LAPSE_ACTIVE_CHANGED = 2,  // another active element
    LAPSE_DOC_CHANGED = 4,     // the document changed (another revision)
    LAPSE_MODE_CHANGED = 8,    // another mode of the window
    LAPSE_SAVED = 16,          // the document has a file
};
// Conditions of a message by its text: the table of conditional messages (app-ux §9.1 «Lapse»); others — LAPSE_NONE.
unsigned toastLapseOf(const QString& text);
// The message posted at `at` lost its sense in `now`.
bool toastLapsed(unsigned lapse, const ToastState& at, const ToastState& now);

class ToastHost : public QWidget {
    Q_OBJECT
public:
    explicit ToastHost(QWidget* view);
    void post(const QString& text, bool error = false, std::vector<ToastAction> actions = {});
    // A conditional message (S7F-UI-003): closed by recheck once lapsed against the state at posting.
    void post(const QString& text, bool error, std::vector<ToastAction> actions, unsigned lapse, const ToastState& at);
    void recheck(const ToastState& now);
    QStringList texts() const;   // texts of visible notifications (test mode)
    // Every notification posted since the last clear(), visible or already closed (test mode: an informational one
    // closes after TOAST_INFO_MS, which a slow check may outlive).
    const QStringList& history() const { return history_; }
    void clear();
    void relayout();             // at the parent's bottom-right corner

private:
    QWidget* add(const QString& text, bool error, std::vector<ToastAction> actions);  // the new frame
    QStringList history_;
    struct Conditional {
        QPointer<QWidget> frame;  // deleted with the oldest beyond TOAST_MAX or by its cross
        unsigned lapse;
        ToastState at;
    };
    std::vector<Conditional> conditional_;
};

}  // namespace construe::app
