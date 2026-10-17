#include "request_handler.h"

#include <catch2/catch_test_macros.hpp>

using namespace chatroom;
using nlohmann::json;

TEST_CASE("Ping receives a correlated pong response", "[protocol][handler]") {
    const RequestHandler handler;
    const auto response = handler.handle(
        R"({"type":"request","request_id":"ping-1","action":"ping","data":{}})");
    CHECK(response["type"] == "response");
    CHECK(response["request_id"] == "ping-1");
    CHECK(response["action"] == "ping");
    CHECK(response["ok"] == true);
    CHECK(response["data"] == json{{"pong", true}});
    CHECK_FALSE(response.contains("error"));
}

TEST_CASE("Unsupported actions return an error without claiming business support",
          "[protocol][handler]") {
    const RequestHandler handler;
    for (const auto* action : {"auth", "send_message", "create_group", "unknown"}) {
        const json request{{"type", "request"}, {"request_id", "r1"}, {"action", action}};
        const auto response = handler.handle(request.dump());
        CHECK(response["request_id"] == "r1");
        CHECK(response["action"] == action);
        CHECK(response["ok"] == false);
        CHECK(response["error"]["code"] == "unknown_action");
        CHECK_FALSE(response.contains("data"));
    }
}

TEST_CASE("Malformed JSON receives a structured protocol error", "[protocol][handler]") {
    const RequestHandler handler;
    for (const auto& payload : {std::string("{broken"), std::string(1, '\xff')}) {
        const auto response = handler.handle(payload);
        CHECK(response["type"] == "response");
        CHECK(response["request_id"].is_null());
        CHECK(response["action"].is_null());
        CHECK(response["ok"] == false);
        CHECK(response["error"]["code"] == "invalid_json");
    }
}

TEST_CASE("Invalid operation data retains the request identity", "[protocol][handler]") {
    const RequestHandler handler;
    const auto response = handler.handle(
        R"({"type":"request","request_id":"r1","action":"ping","data":null})");
    CHECK(response["request_id"] == "r1");
    CHECK(response["action"] == "ping");
    CHECK(response["ok"] == false);
    CHECK(response["error"]["code"] == "invalid_request");
}

TEST_CASE("Invalid metadata is not reflected into error responses", "[protocol][handler]") {
    const RequestHandler handler;
    const json request{{"type", "request"}, {"request_id", std::string(65, 'x')},
                       {"action", "ping"}};
    const auto response = handler.handle(request.dump());
    CHECK(response["request_id"].is_null());
    CHECK(response["action"] == "ping");
    CHECK(response["error"]["code"] == "invalid_request");
}
