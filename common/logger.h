#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace chatroom {

enum class LogLevel { debug, info, warn, error };

LogLevel parse_log_level(std::string_view text);
void set_log_level(LogLevel level);

// Writes one JSON line to standard error. Safe to call from any thread.
// Context must contain metadata only, never passwords, tokens, or request bodies.
void log(LogLevel level, std::string_view event,
         nlohmann::json context = nlohmann::json::object());

} // namespace chatroom
