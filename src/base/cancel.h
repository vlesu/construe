// Cancellation flag for long operations (changed from another thread).
#pragma once

#include <atomic>
#include <chrono>
#include <optional>

namespace construe {

struct Cancel {
    std::atomic<bool> requested{false};
    // Progress of a long operation (core-api §4.7з, FX7-UI-006): steps done of total; total 0 — unknown.
    // Written by the operation through a const pointer.
    mutable std::atomic<int> done{0}, total{0};
    // A flag for one call inside a longer operation: it is also up when the operation is cancelled (parent) and when the
    // time of the call has run out (deadline).
    const Cancel* parent = nullptr;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool isRequested() const {
        return requested.load(std::memory_order_relaxed) || (parent && parent->isRequested()) || (deadline && std::chrono::steady_clock::now() >= *deadline);
    }
};

inline bool cancelled(const Cancel* c) { return c && c->isRequested(); }

inline void reportProgress(const Cancel* c, int done, int total) {
    if (!c) return;
    c->total.store(total, std::memory_order_relaxed);
    c->done.store(done, std::memory_order_relaxed);
}

}  // namespace construe
