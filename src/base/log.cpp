#include "base/log.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace construe::log {

namespace {
std::mutex g_mutex;
Sink g_sink;
}  // namespace

void setSink(Sink sink) {
    std::lock_guard lock(g_mutex);
    g_sink = std::move(sink);
}

void write(Level level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    if (g_sink) {
        g_sink(level, message);
        return;
    }
    if (level >= Level::Warn) std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
}

}  // namespace construe::log
