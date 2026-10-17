#include "logger.h"

#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace chatroom;

namespace {
class CapturedLogs {
public:
    CapturedLogs() : original_(std::clog.rdbuf(output.rdbuf())) { set_log_level(LogLevel::info); }
    ~CapturedLogs() {
        std::clog.rdbuf(original_);
        set_log_level(LogLevel::info);
    }

    std::ostringstream output;

private:
    std::streambuf* original_;
};
} // namespace

TEST_CASE("Logs keep escaped error details on a single JSON line", "[logger]") {
    CapturedLogs capture;
    log(LogLevel::warn, "request_error",
        {{"connection_id", 42}, {"request_id", "r1"}, {"error", "First line\nSecond line"}});
    std::istringstream lines(capture.output.str());
    std::string line;
    REQUIRE(std::getline(lines, line));
    const auto entry = nlohmann::json::parse(line);
    CHECK(entry.at("level") == "warn");
    CHECK(entry.at("event") == "request_error");
    CHECK(entry.at("context").at("connection_id") == 42);
    CHECK(entry.at("context").at("request_id") == "r1");
    CHECK(entry.at("context").at("error") == "First line\nSecond line");
    const auto timestamp = entry.at("timestamp").get<std::string>();
    CHECK(timestamp.size() == 24);
    CHECK(timestamp.back() == 'Z');
    CHECK_FALSE(std::getline(lines, line));
}

TEST_CASE("The log threshold filters less severe records", "[logger]") {
    CapturedLogs capture;
    set_log_level(parse_log_level("error"));
    log(LogLevel::debug, "hidden_debug");
    log(LogLevel::info, "hidden_info");
    log(LogLevel::warn, "hidden_warning");
    log(LogLevel::error, "visible_error");
    const auto entry = nlohmann::json::parse(capture.output.str());
    CHECK(entry.at("event") == "visible_error");
    CHECK(entry.at("level") == "error");
}

TEST_CASE("Concurrent workers emit complete JSON log records", "[logger]") {
    CapturedLogs capture;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([worker] {
            for (int request = 0; request < 10; ++request) {
                log(LogLevel::info, "worker_record", {{"worker", worker}, {"request", request}});
            }
        });
    }
    for (auto& worker : workers) worker.join();
    std::istringstream lines(capture.output.str());
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        const auto entry = nlohmann::json::parse(line);
        CHECK(entry.at("event") == "worker_record");
        ++count;
    }
    CHECK(count == 40);
}
