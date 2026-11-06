// Session worker thread (spec/core-api.md §3): queue of jobs (regeneration, preview, export),
// executed one at a time in submission order; waiting for idle.
#pragma once

#include "base/cancel.h"
#include "base/thread.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace construe::api {

struct Job {
    enum class Kind { Regen, Preview, Export };
    Kind kind = Kind::Regen;
    std::function<void(const Cancel*)> run;
    std::shared_ptr<Cancel> cancel;
};

class JobRunner {
public:
    JobRunner();
    ~JobRunner();  // stop()
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    void enqueue(Job job);
    // The queue is empty and no job is running; false - the timeout expired.
    bool waitIdle(int timeoutMs);
    // Stop the thread: pending jobs are discarded, the current one runs to completion (its cancellation is requested
    // by the owner before the call). A repeated call has no effect.
    void stop();

private:
    void loop();

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::deque<Job> queue_;
    bool busy_ = false;
    bool stop_ = false;
    WorkerThread worker_;  // explicit stack size: kernel recursion on macOS (base/thread.h)
};

}  // namespace construe::api
