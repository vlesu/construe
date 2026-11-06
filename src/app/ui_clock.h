// Clock of the interface delays (spec/app-ux.md §12.1, the replaceable clock; FX14-CI-002 item D): the delays that are the
// behavior of the program (the long-work stripe after BUSY_SHOW_DELAY_MS, app-ux §9.4) count by this clock. Normally it
// is the monotonic wall clock; a test switches it to manual time and advances it — the delay is checked without
// waiting for it in real time.
#pragma once

#include <QObject>
#include <QtGlobal>

#include <functional>

namespace construe::app {

class UiClock {
public:
    // Milliseconds since an arbitrary start; monotonic.
    static qint64 nowMs();
    // Tests (§12.1): manual time — nowMs() stands still until advanceForTest(); off — back to the wall clock, going on
    // from the manual value (never backwards).
    static void setManualForTest(bool manual);
    static bool manualForTest();
    // Moves the manual time forward and lets the listeners (the stripe's poll) see the new time at once.
    static void advanceForTest(qint64 ms);
    // A delayed call by this clock: a QTimer single shot on the wall clock; in manual time — when an advance reaches it
    // (in the order of the due times). Not called if `context` is destroyed first.
    static void singleShot(int ms, QObject* context, std::function<void()> fn);
    // A listener called after every advance; returns an id for removeListener.
    static int addListener(std::function<void()> fn);
    static void removeListener(int id);
};

}  // namespace construe::app
