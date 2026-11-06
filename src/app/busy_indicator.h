// Long work indicator (spec/app-ux.md §9.4, FX8-UI-001): a thin running stripe at the top of the main window while
// the program works longer than BUSY_SHOW_DELAY_MS — regeneration, preview, export or work in the interface thread.
#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace construe::app {

// The show delay without windows: shown once the work has been going on continuously for the delay; a poll without
// work hides at once.
class BusyDelay {
public:
    explicit BusyDelay(qint64 delayMs) : delayMs_(delayMs) {}
    // Returns whether the indicator is shown after this poll.
    bool update(bool busy, qint64 nowMs);
    bool shown() const { return shown_; }

private:
    qint64 delayMs_;
    qint64 since_ = -1;
    bool shown_ = false;
};

// The stripe: an overlay child of the window's central area (not in its layout), indeterminate progress.
class BusyBar : public QWidget {
public:
    explicit BusyBar(QWidget* parent);
    void start();  // phase from now
    // Position of the running segment's left edge in widths (−fraction…1) at elapsed ms (app-ux §9.4).
    static double segmentStart(qint64 elapsedMs);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    qint64 startMs_ = 0;  // UiClock time of the phase start
};

class BusyIndicator : public QObject {
public:
    // busy — whether asynchronous work goes on (regeneration, preview, export); host — the central widget of the window.
    BusyIndicator(QWidget* host, std::function<bool()> busy);
    ~BusyIndicator() override;
    BusyBar* bar() const { return bar_; }
    int shownCount() const { return shownCount_; }
    // Polls that saw no work (tests, S7F-QA-002): one more after the work ended means the delay is reset and the stripe
    // hidden — the condition to wait for instead of a pause.
    int idlePolls() const { return idlePolls_; }
    void relayout();  // the stripe across the host's top edge

    // Work in the interface thread (nested scopes count).
    class Scope {
    public:
        Scope();
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // Inside a scope: at most once per BUSY_FRAME_MS — poll and repaint only the stripe, without processing events. Not in
    // the interface thread — nothing.
    static void tick();

    // A long single call (FX13-UI-001, app-ux §9.4): `fn` runs in a worker thread inside a scope while the interface
    // thread waits without processing events and ticks; an exception of `fn` is rethrown here. Without an indicator —
    // `fn` is called directly.
    static void run(const std::function<void()>& fn);
    // Tests (§12.1): every run waits this long in the worker thread before `fn`; how many runs had the stripe shown and
    // running (polled while shown) — whether it appeared during the run or before it.
    static void setRunDelayForTest(int ms);
    static int runShownCount();

private:
    void poll();

    QWidget* host_;
    std::function<bool()> busy_;
    BusyBar* bar_;
    QTimer timer_;
    int clockListener_ = 0;  // UiClock listener (manual test time)
    BusyDelay delay_;
    qint64 lastTick_ = -1;
    int shownCount_ = 0;
    int idlePolls_ = 0;
};

using BusyScope = BusyIndicator::Scope;

// busyRun(f) — BusyIndicator::run returning the value of `f` (app-ux §9.4, FX13-UI-001).
template <class F>
auto busyRun(F&& fn) -> std::invoke_result_t<F&> {
    using R = std::invoke_result_t<F&>;
    if constexpr (std::is_void_v<R>) {
        BusyIndicator::run([&] { fn(); });
    } else {
        std::optional<R> r;
        BusyIndicator::run([&] { r.emplace(fn()); });
        return std::move(*r);
    }
}

// Free form of BusyIndicator::tick() for loops outside the window code (SceneBuilder).
inline void busyTick() { BusyIndicator::tick(); }

}  // namespace construe::app
