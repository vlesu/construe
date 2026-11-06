// Bringing the program window to the foreground (app/foreground.h, spec/app-ux.md §2.1, FX7-UI-001).
#include "app/foreground.h"

#include <QApplication>
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QToolTip>
#include <QWidget>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace construe::app {

void bringToFront(QWidget* w) {
    if (!w) return;
    if (w->isMinimized()) w->showNormal();
    w->show();
    w->raise();
    w->activateWindow();
#ifdef _WIN32
    const HWND h = reinterpret_cast<HWND>(w->winId());
    const HWND fg = GetForegroundWindow();
    if (fg == h) return;
    const DWORD self = GetCurrentThreadId();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgThread != 0 && fgThread != self && AttachThreadInput(fgThread, self, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    if (attached) AttachThreadInput(fgThread, self, FALSE);
#endif
}

QStringList shownTopLevelWindows(const QWidget* keep) {
    QStringList out;
    for (QWidget* t : QApplication::topLevelWidgets())
        if (t != keep && t->isVisible() && t->windowType() != Qt::ToolTip)  // tooltips go by themselves
            out << QString("%1 %2 \"%3\"").arg(t->metaObject()->className(), t->objectName(), t->windowTitle());
    return out;
}

void closeOtherTopLevelWindows(const QWidget* keep) {
    QToolTip::hideText();
    QList<QPointer<QWidget>> shown;
    for (QWidget* t : QApplication::topLevelWidgets())
        if (t != keep && t->isVisible()) shown.append(t);
    for (const auto& t : shown) {
        if (!t || !t->isVisible()) continue;  // gone with a window closed before it
        if (auto* d = qobject_cast<QDialog*>(t.data())) d->reject();
        else t->hide();
    }
}

}  // namespace construe::app
