// Long operations with a modal progress window and «Abort» (spec/app-ux.md §9.3, FX7-UI-006): the work
// runs in a worker thread, the interface keeps drawing; abort requests the cancellation and waits for the work to stop.
#pragma once

#include "base/cancel.h"

#include <QString>

#include <functional>

class QWidget;

namespace construe::app {

class LongOperation {
public:
    // Runs work(cancel) in a worker thread. User input is blocked; after LONG_OP_SHOW_DELAY_MS the modal window
    // `longOperationDialog` appears (title, progress, «Abort»). Returns false if the user aborted.
    static bool run(QWidget* parent, const QString& title, const std::function<void(const Cancel*)>& work);
    // e2e (app-ux §12.1): the worker waits before the work until aborted or released — the window is shown for sure.
    static void setHoldForTest(bool hold);
    static void releaseForTest();
};

}  // namespace construe::app
