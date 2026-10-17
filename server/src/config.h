#pragma once

#include "logger.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chatroom {

struct DatabaseConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string name = "chatroom";
    std::string user = "chatroom";
    std::string password;
};

struct ServerConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 8081;
    std::size_t worker_threads = 4;
    std::size_t max_connections = 1024;
    LogLevel log_level = LogLevel::info;
    DatabaseConfig database;
    std::string jwt_secret;
};

// Loads defaults, an optional --config file, environment variables, then CLI overrides.
// Database passwords and JWT secrets are read only from the environment.
ServerConfig load_config(const std::vector<std::string_view>& arguments = {});

} // namespace chatroom
