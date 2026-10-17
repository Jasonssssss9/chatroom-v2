#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chatroom {

// A validated client request. An omitted data field becomes an empty object.
struct Request {
    std::string request_id;
    std::string action;
    nlohmann::json data = nlohmann::json::object();
};

// Retains valid request metadata so malformed requests can receive correlated errors.
struct ProtocolError : std::runtime_error {
    ProtocolError(std::string error_code, const std::string& message, Request context = {})
        : std::runtime_error(message), code(std::move(error_code)), request(std::move(context)) {}

    std::string code;
    Request request;
};

// Parses JSON and validates the envelope. Operation-specific fields belong to handlers.
// Throws ProtocolError for malformed JSON or an invalid request envelope.
Request parse_request(std::string_view payload);

// Builders accept trusted application data and return JSON ready for serialization.
nlohmann::json make_response(const Request& request,
                             nlohmann::json data = nlohmann::json::object());
nlohmann::json make_error_response(const Request& request, std::string code, std::string message);
nlohmann::json make_notification(std::string action, nlohmann::json data);

} // namespace chatroom
