// Worker thread with an explicit stack size (base/thread.h).
#include "base/thread.h"

#include <exception>
#include <system_error>

#ifdef _WIN32
#include <thread>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace construe {

#ifdef _WIN32

struct WorkerThread::State {
    std::thread t;
};

WorkerThread::WorkerThread(std::function<void()> fn, std::size_t) : s_(std::make_unique<State>()) { s_->t = std::thread(std::move(fn)); }

bool WorkerThread::joinable() const noexcept { return s_ && s_->t.joinable(); }

void WorkerThread::join() {
    if (!joinable()) throw std::system_error(std::make_error_code(std::errc::invalid_argument));
    s_->t.join();
}

std::size_t currentThreadStackBytes() { return 0; }

#else

struct WorkerThread::State {
    std::function<void()> fn;
    pthread_t id{};
    bool running = false;
};

namespace {

void* threadMain(void* p) {
    auto& fn = *static_cast<std::function<void()>*>(p);
    try {
        fn();
    } catch (...) {
        std::terminate();  // as std::thread: an exception must not leave the thread function
    }
    return nullptr;
}

}  // namespace

WorkerThread::WorkerThread(std::function<void()> fn, std::size_t stackBytes) : s_(std::make_unique<State>()) {
    s_->fn = std::move(fn);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    // Page multiple (macOS requires it); a size the system rejects — its default stack.
    const long page = sysconf(_SC_PAGESIZE);
    const std::size_t ps = page > 0 ? std::size_t(page) : 4096;
    const std::size_t size = (stackBytes + ps - 1) / ps * ps;
    if (size > 0) pthread_attr_setstacksize(&attr, size);
    int rc = pthread_create(&s_->id, &attr, threadMain, &s_->fn);
    pthread_attr_destroy(&attr);
    if (rc != 0) rc = pthread_create(&s_->id, nullptr, threadMain, &s_->fn);
    if (rc != 0) throw std::system_error(rc, std::generic_category(), "pthread_create");
    s_->running = true;
}

bool WorkerThread::joinable() const noexcept { return s_ && s_->running; }

void WorkerThread::join() {
    if (!joinable()) throw std::system_error(std::make_error_code(std::errc::invalid_argument));
    const int rc = pthread_join(s_->id, nullptr);
    if (rc != 0) throw std::system_error(rc, std::generic_category(), "pthread_join");
    s_->running = false;
}

std::size_t currentThreadStackBytes() {
#if defined(__APPLE__)
    return pthread_get_stacksize_np(pthread_self());
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    std::size_t size = 0;
    pthread_attr_getstacksize(&attr, &size);
    pthread_attr_destroy(&attr);
    return size;
#else
    return 0;
#endif
}

#endif

WorkerThread::WorkerThread() noexcept = default;
WorkerThread::WorkerThread(WorkerThread&&) noexcept = default;

WorkerThread& WorkerThread::operator=(WorkerThread&& o) noexcept {
    if (joinable()) std::terminate();
    s_ = std::move(o.s_);
    return *this;
}

WorkerThread::~WorkerThread() {
    if (joinable()) std::terminate();
}

}  // namespace construe
