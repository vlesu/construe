// Bringing the program window to the foreground at startup (spec/app-ux.md §2.1, FX7-UI-001).
#pragma once

#include <QStringList>

class QWidget;

namespace construe::app {

// Shows the window, raises it above other windows and makes it active. On Windows the system may refuse a process the
// foreground (the foreground lock: e.g. started by a batch file whose console is in front), so the input of the
// current foreground thread is attached for the call.
void bringToFront(QWidget* w);

// Shown top-level windows other than `keep`, as «class objectName "title"» (FX14-QA-008: none after an e2e run);
// tooltips are not counted — they hide by themselves (a hover of the next moment may show one again).
QStringList shownTopLevelWindows(const QWidget* keep);
// Closes every shown top-level window but `keep` (FX14-QA-008, the reset between e2e tests): a dialog — a question in
// its own event loop (QDialog::exec) included — is rejected, other windows (menus, popups, tool windows) are hidden
// without close events.
void closeOtherTopLevelWindows(const QWidget* keep);

}  // namespace construe::app
