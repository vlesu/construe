// Long work indicator (app/busy_indicator.h, spec/app-ux.md §9.4, FX8-UI-001).
#include "app/busy_indicator.h"

#include "app/theme.h"
#include "app/ui_clock.h"
#include "base/thread.h"

#include <QCoreApplication>
#include <QPainter>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace construe::app {

namespace {

// Indicators of all open windows (app-ux §9.4): a long call in the interface thread blocks every window, so every
// window's stripe is polled and painted during it — not only the last window created (a second window opened and closed
// before left the first one's long calls without a stripe, FX14-CI-002).
std::vector<BusyIndicator*>& indicators() {
    static std::vector<BusyIndicator*> v;
    return v;
}
int g_scopes = 0;
int g_runDelayMs = 0;     // tests: the delay of every run (FX13-UI-001)
int g_runShown = 0;       // runs during which the stripe was shown
bool g_inRun = false;
bool g_runSawStripe = false;  // the current run polled with the stripe shown

bool inInterfaceThread() { return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread(); }

}  // namespace

bool BusyDelay::update(bool busy, qint64 nowMs) {
    if (!busy) {
        since_ = -1;
        shown_ = false;
        return false;
    }
    if (since_ < 0) since_ = nowMs;
    shown_ = nowMs - since_ >= delayMs_;
    return shown_;
}

BusyBar::BusyBar(QWidget* parent) : QWidget(parent) {
    setObjectName("busyBar");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedHeight(BUSY_BAR_H);
    startMs_ = UiClock::nowMs();
}

void BusyBar::start() { startMs_ = UiClock::nowMs(); }

double BusyBar::segmentStart(qint64 elapsedMs) {
    const double phase = double(elapsedMs % BUSY_PERIOD_MS) / BUSY_PERIOD_MS;
    return -BUSY_SEGMENT_FRACTION + phase * (1 + BUSY_SEGMENT_FRACTION);
}

void BusyBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    QColor track = COLOR_INFO.color;
    track.setAlpha(BUSY_TRACK_ALPHA);
    p.fillRect(rect(), track);
    const double x0 = segmentStart(UiClock::nowMs() - startMs_) * width();
    p.fillRect(QRectF(x0, 0, BUSY_SEGMENT_FRACTION * width(), height()), COLOR_INFO.color);
}

BusyIndicator::BusyIndicator(QWidget* host, std::function<bool()> busy)
    : QObject(host), host_(host), busy_(std::move(busy)), bar_(new BusyBar(host)), delay_(BUSY_SHOW_DELAY_MS) {
    bar_->hide();
    timer_.setInterval(BUSY_POLL_MS);
    connect(&timer_, &QTimer::timeout, this, &BusyIndicator::poll);
    timer_.start();
    // Manual test time (§12.1): an advance of the clock is seen by a poll at once, not at the next timer tick.
    clockListener_ = UiClock::addListener([this] { poll(); });
    indicators().push_back(this);
}

BusyIndicator::~BusyIndicator() {
    UiClock::removeListener(clockListener_);
    auto& v = indicators();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

void BusyIndicator::relayout() {
    bar_->setGeometry(0, 0, host_->width(), BUSY_BAR_H);
    bar_->raise();
}

void BusyIndicator::poll() {
    const bool was = delay_.shown();
    const bool working = g_scopes > 0 || (busy_ && busy_());
    const bool show = delay_.update(working, UiClock::nowMs());
    if (!working) ++idlePolls_;
    if (show && !was) {
        ++shownCount_;
        relayout();
        bar_->start();
        bar_->show();
        timer_.setInterval(BUSY_FRAME_MS);
    } else if (!show && was) {
        bar_->hide();
        timer_.setInterval(BUSY_POLL_MS);
    }
    if (show) bar_->update();
    // A run counts whether the stripe ran during it, also when it had appeared before the run began — a regeneration
    // longer than the delay just before the scene build (FX14-QA-007: the count of «appeared during a run» missed it).
    if (show && g_inRun) g_runSawStripe = true;
}

BusyIndicator::Scope::Scope() { ++g_scopes; }

BusyIndicator::Scope::~Scope() { --g_scopes; }

void BusyIndicator::tick() {
    if (g_scopes <= 0 || !inInterfaceThread()) return;
    const qint64 now = UiClock::nowMs();
    const auto all = indicators();  // a copy: polling does not open or close windows, but stay safe
    for (BusyIndicator* b : all) {
        if (b->lastTick_ >= 0 && now - b->lastTick_ < BUSY_FRAME_MS) continue;
        b->lastTick_ = now;
        b->poll();
        // The event loop does not run: paint the stripe now (no events are processed — no reentry into commands).
        if (b->delay_.shown()) b->bar_->repaint();
    }
}

void BusyIndicator::run(const std::function<void()>& fn) {
    if (indicators().empty() || !inInterfaceThread() || g_inRun) {  // no window, or already waiting for a run: directly
        fn();
        return;
    }
    const Scope scope;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr error;
    const int delay = g_runDelayMs;
    g_inRun = true;
    g_runSawStripe = false;
    WorkerThread worker([&] {
        try {
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        {
            const std::lock_guard lk(m);
            done = true;
        }
        cv.notify_one();
    });
    {
        std::unique_lock lk(m);
        while (!done) {
            cv.wait_for(lk, std::chrono::milliseconds(BUSY_FRAME_MS));
            if (done) break;
            lk.unlock();
            tick();  // the interface thread only paints the stripe: no events, no reentry
            lk.lock();
        }
    }
    worker.join();
    g_inRun = false;
    if (g_runSawStripe) ++g_runShown;
    if (error) std::rethrow_exception(error);
}

void BusyIndicator::setRunDelayForTest(int ms) { g_runDelayMs = ms; }

int BusyIndicator::runShownCount() { return g_runShown; }

}  // namespace construe::app
