// Worker thread with an explicit stack size (spec/base.md §7д, S8-PKG-003): secondary threads get 512 KiB of stack on
// macOS by default (8 MiB on Linux, 1 MiB on Windows) — too little for the deep recursion of the geometry kernel in
// background regeneration and drawing projection.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace construe {

// Like std::thread (join before destruction, movable), but the stack is at least stackBytes. Windows: std::thread —
// the stack of the executable's default (1 MiB, as before).
class WorkerThread {
public:
    static constexpr std::size_t kDefaultStackBytes = std::size_t(8) << 20;

    WorkerThread() noexcept;
    explicit WorkerThread(std::function<void()> fn, std::size_t stackBytes = kDefaultStackBytes);
    WorkerThread(WorkerThread&&) noexcept;
    WorkerThread& operator=(WorkerThread&&) noexcept;  // the target must not be joinable
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    ~WorkerThread();  // std::terminate if still joinable, as std::thread

    bool joinable() const noexcept;
    void join();

private:
    struct State;
    std::unique_ptr<State> s_;
};

// Stack size of the calling thread in bytes; 0 — unknown on this system (tests).
std::size_t currentThreadStackBytes();

}  // namespace construe
