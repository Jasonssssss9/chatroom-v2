#include "protocol.h"

namespace chatroom {
namespace {
using nlohmann::json;
constexpr int kMaxJsonDepth = 64;

// Keep reflected metadata small, including when reporting an invalid request.
std::string read_name(const json& message, const char* field) {
    const auto it = message.find(field);
    if (it == message.end() || !it->is_string()) {
        return {};
    }
    const auto& value = it->get_ref<const std::string&>();
    return !value.empty() && value.size() <= 64 ? value : std::string{};
}
} // namespace

Request parse_request(std::string_view payload) {
    const auto check_depth = [](int depth, json::parse_event_t event, json&) {
        // Container start events use zero-based depth; the root counts as level one.
        if ((event == json::parse_event_t::object_start ||
             event == json::parse_event_t::array_start) && depth >= kMaxJsonDepth) {
            throw ProtocolError("invalid_request", "JSON nesting exceeds the 64-level limit");
        }
        return true;
    };
    const auto message = json::parse(payload, check_depth, false);
    if (message.is_discarded()) {
        throw ProtocolError("invalid_json", "Payload must contain valid JSON");
    }
    if (!message.is_object()) {
        throw ProtocolError("invalid_request", "Request must be a JSON object");
    }

    Request request;
    request.request_id = read_name(message, "request_id");
    request.action = read_name(message, "action");
    const auto type = message.find("type");
    if (type == message.end() || *type != "request") {
        throw ProtocolError("invalid_request", "type must be request", request);
    }
    if (request.request_id.empty()) {
        throw ProtocolError("invalid_request", "request_id must be a string of 1 to 64 bytes",
                            request);
    }
    if (request.action.empty()) {
        throw ProtocolError("invalid_request", "action must be a string of 1 to 64 bytes", request);
    }
    const auto data = message.find("data");
    if (data != message.end()) {
        if (!data->is_object()) {
            throw ProtocolError("invalid_request", "data must be a JSON object", request);
        }
        request.data = *data;
    }
    return request;
}

nlohmann::json make_response(const Request& request, nlohmann::json data) {
    return {{"type", "response"},
            {"request_id", request.request_id},
            {"action", request.action},
            {"ok", true},
            {"data", std::move(data)}};
}

nlohmann::json make_error_response(const Request& request, std::string code, std::string message) {
    return {{"type", "response"},
            {"request_id", request.request_id.empty() ? json(nullptr) : json(request.request_id)},
            {"action", request.action.empty() ? json(nullptr) : json(request.action)},
            {"ok", false},
            {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}

nlohmann::json make_notification(std::string action, nlohmann::json data) {
    return {{"type", "notification"}, {"action", std::move(action)}, {"data", std::move(data)}};
}

} // namespace chatroom
