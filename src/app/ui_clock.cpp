// Clock of the interface delays (app/ui_clock.h, spec/app-ux.md §12.1, FX14-CI-002).
#include "app/ui_clock.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <map>
#include <vector>

namespace construe::app {

namespace {

struct State {
    QElapsedTimer wall;
    bool manual = false;
    qint64 offset = 0;     // added to the wall clock (keeps the time monotonic after a manual period)
    qint64 manualNow = 0;  // the time while manual
    int nextId = 1;
    std::map<int, std::function<void()>> listeners;
    struct Due {
        qint64 at;
        QPointer<QObject> context;
        std::function<void()> fn;
    };
    std::vector<Due> due;  // manual-time single shots
    State() { wall.start(); }
};

State& st() {
    static State s;
    return s;
}

}  // namespace

qint64 UiClock::nowMs() {
    State& s = st();
    return s.manual ? s.manualNow : s.wall.elapsed() + s.offset;
}

void UiClock::setManualForTest(bool manual) {
    State& s = st();
    if (manual == s.manual) return;
    if (manual) {
        s.manualNow = s.wall.elapsed() + s.offset;
    } else {
        s.offset = s.manualNow - s.wall.elapsed();
        if (s.offset < 0) s.offset = 0;  // the wall clock is already ahead: it goes on from itself
        // Calls still due go on by the wall clock with the time they had left.
        auto due = std::move(s.due);
        s.due.clear();
        for (auto& d : due)
            if (d.context) QTimer::singleShot(int(std::max<qint64>(0, d.at - s.manualNow)), d.context.data(), std::move(d.fn));
    }
    s.manual = manual;
}

void UiClock::singleShot(int ms, QObject* context, std::function<void()> fn) {
    State& s = st();
    if (!s.manual) {
        QTimer::singleShot(ms, context, std::move(fn));
        return;
    }
    s.due.push_back({s.manualNow + std::max(0, ms), QPointer<QObject>(context), std::move(fn)});
}

bool UiClock::manualForTest() { return st().manual; }

void UiClock::advanceForTest(qint64 ms) {
    State& s = st();
    if (!s.manual || ms <= 0) return;
    s.manualNow += ms;
    // Due calls first (the work they finish is seen by the listeners' poll), earliest first; a call may add new ones.
    for (;;) {
        auto it = std::min_element(s.due.begin(), s.due.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
        if (it == s.due.end() || it->at > s.manualNow) break;
        State::Due d = std::move(*it);
        s.due.erase(it);
        if (d.context) d.fn();
    }
    const auto listeners = s.listeners;  // a listener may remove itself
    for (const auto& [id, fn] : listeners) fn();
}

int UiClock::addListener(std::function<void()> fn) {
    State& s = st();
    const int id = s.nextId++;
    s.listeners[id] = std::move(fn);
    return id;
}

void UiClock::removeListener(int id) { st().listeners.erase(id); }

}  // namespace construe::app
