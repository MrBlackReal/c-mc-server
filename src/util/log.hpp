#pragma once

#include <format>
#include <string_view>

namespace mc::log {

enum class Level { Debug, Info, Warn, Error };

void write(Level level, std::string_view message);
void setDebug(bool enabled);
bool debugEnabled();

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (debugEnabled()) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace mc::log
