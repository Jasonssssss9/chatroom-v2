#include "config.h"

#include <arpa/inet.h>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>

namespace chatroom {
namespace {
using Json = nlohmann::json;

std::uint64_t parse_number(std::string_view text, const std::string& name,
                           std::uint64_t minimum, std::uint64_t maximum) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value < minimum || value > maximum) {
        throw std::invalid_argument(name + " must be an integer between " +
                                    std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return value;
}

std::uint64_t read_number(const Json& value, const std::string& name,
                          std::uint64_t minimum, std::uint64_t maximum) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw std::invalid_argument(name + " must be a non-negative integer");
    }
    const auto number = value.get<std::uint64_t>();
    if (number < minimum || number > maximum) {
        throw std::invalid_argument(name + " is out of range");
    }
    return number;
}

std::string read_text(const Json& value, const std::string& name) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        throw std::invalid_argument(name + " must be a non-empty string");
    }
    return value.get<std::string>();
}

Json section(const Json& document, const char* name) {
    auto value = document.value(name, Json::object());
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be an object");
    }
    return value;
}

void read_file(ServerConfig& config, const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open configuration file: " + path);
    }
    Json document;
    try {
        document = Json::parse(file);
    } catch (const Json::parse_error&) {
        // Parser diagnostics may contain file contents, including misplaced secrets.
        throw std::invalid_argument("Invalid JSON in configuration file: " + path);
    }
    if (!document.is_object()) {
        throw std::invalid_argument("Configuration must be a JSON object");
    }
    const auto server = section(document, "server");
    if (server.contains("address")) config.address = read_text(server.at("address"), "server.address");
    if (server.contains("port")) {
        config.port = read_number(server.at("port"), "server.port", 0, 65535);
    }
    if (server.contains("worker_threads")) {
        config.worker_threads = read_number(server.at("worker_threads"), "server.worker_threads",
                                           1, std::numeric_limits<std::size_t>::max());
    }
    if (server.contains("max_connections")) {
        config.max_connections = read_number(server.at("max_connections"), "server.max_connections",
                                             1, std::numeric_limits<std::size_t>::max());
    }
    if (server.contains("max_requests_per_connection")) {
        config.max_requests_per_connection = read_number(
            server.at("max_requests_per_connection"), "server.max_requests_per_connection",
            1, std::numeric_limits<std::size_t>::max());
    }
    if (server.contains("max_pending_requests")) {
        config.max_pending_requests = read_number(server.at("max_pending_requests"),
            "server.max_pending_requests", 1, std::numeric_limits<std::size_t>::max());
    }
    const auto http = section(document, "http");
    if (http.contains("port")) config.http.port = read_number(http.at("port"), "http.port", 0, 65535);
    if (http.contains("worker_threads")) {
        config.http.worker_threads = read_number(http.at("worker_threads"), "http.worker_threads",
                                                 1, std::numeric_limits<std::size_t>::max());
    }
    const auto jwt = section(document, "jwt");
    if (jwt.contains("token_lifetime_seconds")) {
        config.token_lifetime_seconds = read_number(jwt.at("token_lifetime_seconds"),
                                                    "jwt.token_lifetime_seconds", 1, 86400);
    }
    const auto database = section(document, "database");
    if (database.contains("host")) config.database.host = read_text(database.at("host"), "database.host");
    if (database.contains("port")) {
        config.database.port = read_number(database.at("port"), "database.port", 1, 65535);
    }
    if (database.contains("name")) config.database.name = read_text(database.at("name"), "database.name");
    if (database.contains("user")) config.database.user = read_text(database.at("user"), "database.user");
    const auto logging = section(document, "log");
    if (logging.contains("level")) {
        config.log_level = parse_log_level(read_text(logging.at("level"), "log.level"));
    }
}

void read_environment(ServerConfig& config) {
    if (const auto* value = std::getenv("CHATROOM_DB_HOST")) config.database.host = value;
    if (const auto* value = std::getenv("CHATROOM_DB_PORT")) {
        config.database.port = parse_number(value, "CHATROOM_DB_PORT", 1, 65535);
    }
    if (const auto* value = std::getenv("CHATROOM_DB_NAME")) config.database.name = value;
    if (const auto* value = std::getenv("CHATROOM_DB_USER")) config.database.user = value;
    if (const auto* value = std::getenv("CHATROOM_DB_PASSWORD")) config.database.password = value;
    if (const auto* value = std::getenv("CHATROOM_JWT_SECRET")) config.jwt_secret = value;
}
} // namespace

ServerConfig load_config(const std::vector<std::string_view>& arguments) {
    std::optional<std::string> config_path;
    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        const auto option = arguments[i];
        if (option != "--config" && option != "--address" && option != "--port" &&
            option != "--workers" && option != "--max-connections" &&
            option != "--max-requests-per-connection" && option != "--max-pending-requests" &&
            option != "--http-port" && option != "--http-workers" && option != "--log-level") {
            throw std::invalid_argument("Unknown option: " + std::string(option));
        }
        if (i + 1 == arguments.size()) {
            throw std::invalid_argument("Missing value for " + std::string(option));
        }
        if (option == "--config") config_path = arguments[i + 1];
    }

    ServerConfig config;
    if (config_path) read_file(config, *config_path);
    read_environment(config);

    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        const auto option = arguments[i];
        const auto value = arguments[i + 1];
        if (option == "--address") config.address = value;
        if (option == "--port") config.port = parse_number(value, "--port", 0, 65535);
        if (option == "--http-port") config.http.port = parse_number(value, "--http-port", 0, 65535);
        if (option == "--http-workers") {
            config.http.worker_threads = parse_number(value, "--http-workers", 1,
                                                       std::numeric_limits<std::size_t>::max());
        }
        if (option == "--workers") {
            config.worker_threads = parse_number(value, "--workers", 1,
                                                 std::numeric_limits<std::size_t>::max());
        }
        if (option == "--max-connections") {
            config.max_connections = parse_number(value, "--max-connections", 1,
                                                   std::numeric_limits<std::size_t>::max());
        }
        if (option == "--max-requests-per-connection") {
            config.max_requests_per_connection = parse_number(value, "--max-requests-per-connection",
                                                               1, std::numeric_limits<std::size_t>::max());
        }
        if (option == "--max-pending-requests") {
            config.max_pending_requests = parse_number(value, "--max-pending-requests", 1,
                                                       std::numeric_limits<std::size_t>::max());
        }
        if (option == "--log-level") config.log_level = parse_log_level(value);
    }

    in_addr address{};
    if (::inet_pton(AF_INET, config.address.c_str(), &address) != 1) {
        throw std::invalid_argument("Server address must be a numeric IPv4 address");
    }
    if (config.database.host.empty() || config.database.name.empty() || config.database.user.empty()) {
        throw std::invalid_argument("Database host, name, and user must not be empty");
    }
    return config;
}

} // namespace chatroom
