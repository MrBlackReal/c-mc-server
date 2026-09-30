#include "util/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace mc::log {
namespace {
std::mutex g_mutex;
bool g_debug = false;

const char* levelName(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "?";
}
} // namespace

void setDebug(bool enabled) { g_debug = enabled; }
bool debugEnabled() { return g_debug; }

void write(Level level, std::string_view message) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[16];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    std::lock_guard lock(g_mutex);
    std::fprintf(level >= Level::Warn ? stderr : stdout, "[%s] [Server thread/%s]: %.*s\n", stamp, levelName(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stdout);
}

} // namespace mc::log
