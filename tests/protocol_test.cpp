#include "protocol.h"

#include <catch2/catch_test_macros.hpp>

using namespace chatroom;
using nlohmann::json;

TEST_CASE("Requests contain an ID, action, and object data", "[protocol]") {
    const auto request = parse_request(
        R"({"type":"request","request_id":"r1","action":"ping","data":{"extra":1}})");
    CHECK(request.request_id == "r1");
    CHECK(request.action == "ping");
    CHECK(request.data == json{{"extra", 1}});
}

TEST_CASE("Omitted request data defaults to an empty object", "[protocol]") {
    const auto request = parse_request(
        R"({"type":"request","request_id":"r1","action":"ping","extra":"ignored"})");
    CHECK(request.data.is_object());
    CHECK(request.data.empty());
}

TEST_CASE("Request parsing rejects malformed JSON and non-object roots", "[protocol]") {
    for (const auto* payload : {"", "{broken", "[]", "null", "42", "true", "\"ping\""}) {
        INFO(payload);
        CHECK_THROWS_AS(parse_request(payload), ProtocolError);
    }
}

TEST_CASE("Required request fields have explicit types and byte limits", "[protocol]") {
    const json valid{{"type", "request"}, {"request_id", "r1"}, {"action", "ping"}};
    for (const auto* field : {"request_id", "action"}) {
        INFO(field);
        auto message = valid;
        message.erase(field);
        CHECK_THROWS_AS(parse_request(message.dump()), ProtocolError);
        for (const auto& value : {json(nullptr), json(1), json(true), json::array(), json::object(),
                                  json(""), json(std::string(65, 'x'))}) {
            message = valid;
            message[field] = value;
            CHECK_THROWS_AS(parse_request(message.dump()), ProtocolError);
        }
        message = valid;
        message[field] = std::string(64, 'x');
        CHECK_NOTHROW(parse_request(message.dump()));
    }
}

TEST_CASE("Only request envelopes with object data are accepted", "[protocol]") {
    const json valid{{"type", "request"}, {"request_id", "r1"}, {"action", "ping"}};
    auto message = valid;
    message.erase("type");
    CHECK_THROWS_AS(parse_request(message.dump()), ProtocolError);
    for (const auto& type : {json(nullptr), json(1), json("response"), json("notification")}) {
        message = valid;
        message["type"] = type;
        CHECK_THROWS_AS(parse_request(message.dump()), ProtocolError);
    }
    for (const auto& data : {json(nullptr), json(1), json(true), json("text"), json::array()}) {
        message = valid;
        message["data"] = data;
        CHECK_THROWS_AS(parse_request(message.dump()), ProtocolError);
    }
}

TEST_CASE("Protocol errors retain only valid correlation metadata", "[protocol]") {
    try {
        parse_request(R"({"type":"request","request_id":42,"action":"ping"})");
        FAIL("Invalid request_id was accepted");
    } catch (const ProtocolError& error) {
        CHECK(error.code == "invalid_request");
        CHECK(error.request.request_id.empty());
        CHECK(error.request.action == "ping");
    }
}

TEST_CASE("Success responses identify the request and contain data", "[protocol]") {
    const Request request{"r1", "ping"};
    const auto response = make_response(request, {{"pong", true}});
    const json expected{{"type", "response"}, {"request_id", "r1"}, {"action", "ping"},
                        {"ok", true}, {"data", {{"pong", true}}}};
    CHECK(response == expected);
    CHECK(make_response(request)["data"] == json::object());
}

TEST_CASE("Error responses use null for unavailable correlation metadata", "[protocol]") {
    const auto response =
        make_error_response({}, "invalid_json", "Payload must contain valid JSON");
    const json expected{{"type", "response"}, {"request_id", nullptr}, {"action", nullptr},
                        {"ok", false},
                        {"error", {{"code", "invalid_json"},
                                   {"message", "Payload must contain valid JSON"}}}};
    CHECK(response == expected);
}

TEST_CASE("Notifications are independent of client requests", "[protocol]") {
    const auto notification = make_notification("message", {{"message_id", "m1"}});
    const json expected{{"type", "notification"}, {"action", "message"},
                        {"data", {{"message_id", "m1"}}}};
    CHECK(notification == expected);
    CHECK_FALSE(notification.contains("request_id"));
    CHECK_FALSE(notification.contains("ok"));
}
