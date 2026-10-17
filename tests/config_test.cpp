#include "config.h"
#include "unique_fd.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace chatroom;

namespace {
// Preserve the developer's environment and keep configuration tests independent.
class TestEnvironment {
public:
    TestEnvironment() {
        for (const auto* name : {"CHATROOM_DB_HOST", "CHATROOM_DB_PORT", "CHATROOM_DB_NAME",
                                 "CHATROOM_DB_USER", "CHATROOM_DB_PASSWORD", "CHATROOM_JWT_SECRET"}) {
            const auto* value = std::getenv(name);
            saved_.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
            ::unsetenv(name);
        }
    }

    ~TestEnvironment() {
        for (const auto& [name, value] : saved_) {
            if (value) ::setenv(name.c_str(), value->c_str(), 1);
            else ::unsetenv(name.c_str());
        }
    }

    void set(const char* name, const char* value) {
        REQUIRE(::setenv(name, value, 1) == 0);
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

class ConfigFile {
public:
    explicit ConfigFile(std::string_view contents) {
        char name[] = "/tmp/chatroom-config-XXXXXX";
        UniqueFd descriptor(::mkstemp(name));
        if (descriptor.get() < 0) throw std::runtime_error("Cannot create test configuration");
        path = name;
        std::ofstream file(path);
        file << contents;
        REQUIRE(file.good());
    }

    ~ConfigFile() { std::remove(path.c_str()); }

    std::string path;
};
} // namespace

TEST_CASE("The server starts with useful defaults and no credentials", "[config]") {
    TestEnvironment environment;
    const auto config = load_config();
    CHECK(config.address == "127.0.0.1");
    CHECK(config.port == 8081);
    CHECK(config.worker_threads == 4);
    CHECK(config.max_connections == 1024);
    CHECK(config.log_level == LogLevel::info);
    CHECK(config.database.host == "127.0.0.1");
    CHECK(config.database.port == 3306);
    CHECK(config.database.name == "chatroom");
    CHECK(config.database.user == "chatroom");
    CHECK(config.database.password.empty());
    CHECK(config.jwt_secret.empty());
}

TEST_CASE("A JSON file configures the server, database, and log level", "[config]") {
    TestEnvironment environment;
    ConfigFile file(R"({
        "server": {"address":"0.0.0.0", "port":9000, "worker_threads":2, "max_connections":50},
        "database": {"host":"mysql", "port":3307, "name":"test_chat", "user":"tester"},
        "log": {"level":"warn"}
    })");
    const auto config = load_config({"--config", file.path});
    CHECK(config.address == "0.0.0.0");
    CHECK(config.port == 9000);
    CHECK(config.worker_threads == 2);
    CHECK(config.max_connections == 50);
    CHECK(config.log_level == LogLevel::warn);
    CHECK(config.database.host == "mysql");
    CHECK(config.database.port == 3307);
    CHECK(config.database.name == "test_chat");
    CHECK(config.database.user == "tester");
}

TEST_CASE("CLI values override the file regardless of option order", "[config]") {
    TestEnvironment environment;
    ConfigFile file(R"({"server":{"port":9000,"worker_threads":8,"max_connections":50},"log":{"level":"error"}})");
    const auto config = load_config({"--port", "0", "--workers", "2", "--config", file.path,
                                     "--address", "0.0.0.0", "--log-level", "debug",
                                     "--max-connections", "20"});
    CHECK(config.address == "0.0.0.0");
    CHECK(config.port == 0);
    CHECK(config.worker_threads == 2);
    CHECK(config.max_connections == 20);
    CHECK(config.log_level == LogLevel::debug);
    CHECK(config.database.port == 3306);
}

TEST_CASE("Environment variables override database settings and supply secrets", "[config]") {
    TestEnvironment environment;
    ConfigFile file(R"({"database":{"host":"from-file","port":3307,"name":"old","user":"old"}})");
    environment.set("CHATROOM_DB_HOST", "from-environment");
    environment.set("CHATROOM_DB_PORT", "3308");
    environment.set("CHATROOM_DB_NAME", "environment_chat");
    environment.set("CHATROOM_DB_USER", "environment_user");
    environment.set("CHATROOM_DB_PASSWORD", "test-password");
    environment.set("CHATROOM_JWT_SECRET", "test-jwt-secret");
    const auto config = load_config({"--config", file.path});
    CHECK(config.database.host == "from-environment");
    CHECK(config.database.port == 3308);
    CHECK(config.database.name == "environment_chat");
    CHECK(config.database.user == "environment_user");
    CHECK(config.database.password == "test-password");
    CHECK(config.jwt_secret == "test-jwt-secret");
}

TEST_CASE("Invalid JSON settings fail before server startup", "[config]") {
    TestEnvironment environment;
    for (const auto* contents : {"{", "[]", R"({"server":null})",
                                 R"({"server":{"port":-1}})", R"({"server":{"port":65536}})",
                                 R"({"server":{"port":1.5}})", R"({"server":{"port":"8081"}})",
                                 R"({"server":{"worker_threads":0}})", R"({"database":{"port":0}})",
                                 R"({"server":{"max_connections":0}})",
                                 R"({"server":{"max_connections":"50"}})",
                                 R"({"database":{"host":""}})", R"({"log":{"level":"verbose"}})"}) {
        INFO(contents);
        ConfigFile file(contents);
        CHECK_THROWS_AS(load_config({"--config", file.path}), std::invalid_argument);
    }
    CHECK_THROWS_AS(load_config({"--config", "/tmp/chatroom-config-missing/server.json"}),
                    std::runtime_error);
}

TEST_CASE("Invalid CLI options and ranges fail before server startup", "[config]") {
    TestEnvironment environment;
    CHECK_THROWS_AS(load_config({"--unknown", "value"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--port"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--port", "65536"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--port", "-1"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--port", "80extra"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--workers", "0"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--workers", "1.5"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--max-connections", "0"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--max-connections", "-1"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--address", "localhost"}), std::invalid_argument);
    CHECK_THROWS_AS(load_config({"--log-level", "verbose"}), std::invalid_argument);
}

TEST_CASE("Invalid database environment variables report their source", "[config]") {
    TestEnvironment environment;
    environment.set("CHATROOM_DB_PORT", "wrong");
    CHECK_THROWS_WITH(load_config(), "CHATROOM_DB_PORT must be an integer between 1 and 65535");
    environment.set("CHATROOM_DB_PORT", "3306");
    environment.set("CHATROOM_DB_USER", "");
    CHECK_THROWS_WITH(load_config(), "Database host, name, and user must not be empty");
}

TEST_CASE("Malformed configuration errors do not echo file contents", "[config]") {
    TestEnvironment environment;
    ConfigFile file(R"({"password":"sensitive-value",)");
    CHECK_THROWS_WITH(load_config({"--config", file.path}),
                      "Invalid JSON in configuration file: " + file.path);
}
