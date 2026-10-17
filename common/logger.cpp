#include "logger.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace chatroom {
namespace {
std::atomic<LogLevel> minimum_level{LogLevel::info};
std::mutex output_mutex;

const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warn: return "warn";
    case LogLevel::error: return "error";
    }
    return "error";
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S.")
         << std::setfill('0') << std::setw(3) << milliseconds << 'Z';
    return text.str();
}
} // namespace

LogLevel parse_log_level(std::string_view text) {
    if (text == "debug") return LogLevel::debug;
    if (text == "info") return LogLevel::info;
    if (text == "warn") return LogLevel::warn;
    if (text == "error") return LogLevel::error;
    throw std::invalid_argument("Log level must be debug, info, warn, or error");
}

void set_log_level(LogLevel level) {
    minimum_level = level;
}

void log(LogLevel level, std::string_view event, nlohmann::json context) {
    if (level < minimum_level.load()) {
        return;
    }
    const nlohmann::json entry = {
        {"timestamp", timestamp()}, {"level", level_name(level)},
        {"event", event}, {"context", std::move(context)}
    };
    // Replace invalid UTF-8 in system error text while keeping each record valid JSON.
    const auto line = entry.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::lock_guard<std::mutex> lock(output_mutex);
    std::clog << line << std::endl;
}

} // namespace chatroom
