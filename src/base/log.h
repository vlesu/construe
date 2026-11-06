// Log (spec/base.md §7). The sink is set by the application; by default warn+ to stderr.
#pragma once

#include <functional>
#include <string_view>

namespace construe::log {

enum class Level { Trace, Debug, Info, Warn, Error };

using Sink = std::function<void(Level, std::string_view)>;

void setSink(Sink sink);
void write(Level level, std::string_view message);

inline void warn(std::string_view m) { write(Level::Warn, m); }
inline void error(std::string_view m) { write(Level::Error, m); }
inline void info(std::string_view m) { write(Level::Info, m); }

}  // namespace construe::log
