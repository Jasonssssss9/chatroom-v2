#include "request_handler.h"

#include <catch2/catch_test_macros.hpp>

using namespace chatroom;
using nlohmann::json;

namespace {
const TokenService kTestTokens(std::string(32, 'k'));
}

TEST_CASE("Ping receives a correlated pong response", "[protocol][handler]") {
    const RequestHandler handler(kTestTokens, {});
    const auto response = handler.handle(
        R"({"type":"request","request_id":"ping-1","action":"ping","data":{}})").response;
    CHECK(response["type"] == "response");
    CHECK(response["request_id"] == "ping-1");
    CHECK(response["action"] == "ping");
    CHECK(response["ok"] == true);
    CHECK(response["data"] == json{{"pong", true}});
    CHECK_FALSE(response.contains("error"));
}

TEST_CASE("Unsupported actions return an error without claiming business support",
          "[protocol][handler]") {
    const RequestHandler handler(kTestTokens, {});
    const Session session{42, std::chrono::system_clock::now() + std::chrono::hours(1)};
    for (const auto* action : {"auth", "create_group", "unknown"}) {
        const json request{{"type", "request"}, {"request_id", "r1"}, {"action", action}};
        const auto response = handler.handle(request.dump(), session).response;
        CHECK(response["request_id"] == "r1");
        CHECK(response["action"] == action);
        CHECK(response["ok"] == false);
        CHECK(response["error"]["code"] == "unknown_action");
        CHECK_FALSE(response.contains("data"));
    }
}

TEST_CASE("Malformed JSON receives a structured protocol error", "[protocol][handler]") {
    const RequestHandler handler(kTestTokens, {});
    for (const auto& payload : {std::string("{broken"), std::string(1, '\xff')}) {
        const auto response = handler.handle(payload).response;
        CHECK(response["type"] == "response");
        CHECK(response["request_id"].is_null());
        CHECK(response["action"].is_null());
        CHECK(response["ok"] == false);
        CHECK(response["error"]["code"] == "invalid_json");
    }
}

TEST_CASE("Invalid operation data retains the request identity", "[protocol][handler]") {
    const RequestHandler handler(kTestTokens, {});
    const auto response = handler.handle(
        R"({"type":"request","request_id":"r1","action":"ping","data":null})").response;
    CHECK(response["request_id"] == "r1");
    CHECK(response["action"] == "ping");
    CHECK(response["ok"] == false);
    CHECK(response["error"]["code"] == "invalid_request");
}

TEST_CASE("Invalid metadata is not reflected into error responses", "[protocol][handler]") {
    const RequestHandler handler(kTestTokens, {});
    const json request{{"type", "request"}, {"request_id", std::string(65, 'x')},
                       {"action", "ping"}};
    const auto response = handler.handle(request.dump()).response;
    CHECK(response["request_id"].is_null());
    CHECK(response["action"] == "ping");
    CHECK(response["error"]["code"] == "invalid_request");
}

TEST_CASE("Unauthenticated and expired sessions cannot access business actions", "[handler][auth]") {
    const RequestHandler handler(kTestTokens, {});
    const Request request{"r1", "send_message"};
    CHECK(handler.handle(request).response["error"]["code"] == "authentication_required");
    const Session expired{42, std::chrono::system_clock::now() - std::chrono::seconds(1)};
    CHECK(handler.handle(request, expired).response["error"]["code"] == "authentication_required");
    CHECK(handler.handle(Request{"r2", "ping"}, expired).response["data"]["pong"] == true);
}

TEST_CASE("TCP authentication validates token input before accessing MySQL", "[handler][auth]") {
    const RequestHandler handler(kTestTokens, {});
    for (const auto& data : {json::object(), json{{"token", 42}}, json{{"token", ""}},
                             json{{"token", std::string(4097, 'x')}}}) {
        const auto result = handler.handle(Request{"r1", "authenticate", data});
        CHECK(result.response["error"]["code"] == "invalid_request");
        CHECK_FALSE(result.authenticated_session);
    }
    const auto result = handler.handle(Request{"r2", "authenticate", {{"token", "invalid-token"}}});
    CHECK(result.response["error"]["code"] == "invalid_token");
    CHECK_FALSE(result.authenticated_session);
}

TEST_CASE("A connection must reconnect to switch users", "[handler][auth]") {
    const RequestHandler handler(kTestTokens, {});
    const Session session{7, std::chrono::system_clock::now() + std::chrono::hours(1)};
    const auto result = handler.handle(
        Request{"r1", "authenticate", {{"token", kTestTokens.issue(8)}}}, session);
    CHECK(result.response["error"]["code"] == "already_authenticated");
    CHECK_FALSE(result.authenticated_session);
}

TEST_CASE("Message input is validated before accessing MySQL", "[handler][message]") {
    const RequestHandler handler(kTestTokens, {});
    const Session session{7, std::chrono::system_clock::now() + std::chrono::hours(1)};
    const json valid{{"client_message_id", "m1"}, {"recipients", {"8"}}, {"text", "Hello"}};
    std::vector<json> invalid{json::object()};
    for (const auto* field : {"client_message_id", "recipients", "text"}) {
        auto data = valid;
        data.erase(field);
        invalid.push_back(std::move(data));
        data = valid;
        data[field] = nullptr;
        invalid.push_back(std::move(data));
    }
    for (const auto& value : {json(""), json(42), json(std::string(65, 'x'))}) {
        auto data = valid;
        data["client_message_id"] = value;
        invalid.push_back(std::move(data));
    }
    for (const auto& value : {json(""), json(42), json(std::string(4097, 'x'))}) {
        auto data = valid;
        data["text"] = value;
        invalid.push_back(std::move(data));
    }
    for (const auto& value : {json::array(), json::object(), json("8"),
                              json(std::vector<std::string>(65, "8")), json::array({8}),
                              json::array({"0"}), json::array({""}), json::array({"08"}),
                              json::array({"+8"}), json::array({"-8"}), json::array({"8tail"}),
                              json::array({"18446744073709551616"})}) {
        auto data = valid;
        data["recipients"] = value;
        invalid.push_back(std::move(data));
    }
    for (const auto& data : invalid) {
        const auto result = handler.handle(Request{"r1", "send_message", data}, session);
        CHECK(result.response["request_id"] == "r1");
        CHECK(result.response["action"] == "send_message");
        CHECK(result.response["error"]["code"] == "invalid_request");
        CHECK_FALSE(result.notification);
    }
}
