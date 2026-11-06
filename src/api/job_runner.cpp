// Session worker thread (api/job_runner.h).
#include "api/job_runner.h"

#include <chrono>

namespace construe::api {

JobRunner::JobRunner() : worker_([this] { loop(); }) {}

JobRunner::~JobRunner() { stop(); }

void JobRunner::stop() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void JobRunner::enqueue(Job job) {
    {
        std::lock_guard lk(mu_);
        queue_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void JobRunner::loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        job.run(job.cancel.get());
        {
            std::lock_guard lk(mu_);
            busy_ = false;
        }
        idleCv_.notify_all();
    }
}

bool JobRunner::waitIdle(int timeoutMs) {
    std::unique_lock lk(mu_);
    return idleCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return queue_.empty() && !busy_; });
}

}  // namespace construe::api
