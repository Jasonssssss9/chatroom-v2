#include "auth_client.h"
#include "protocol.h"

#include <stdexcept>

namespace chatroom {

AuthClient::AuthClient(const std::string& address, std::uint16_t port) : client_(address, port) {
    client_.set_connection_timeout(5);
    client_.set_read_timeout(5);
    client_.set_write_timeout(5);
    client_.set_max_timeout(10000);
    client_.set_payload_max_length(8192);
}

nlohmann::json AuthClient::post(const char* path, const std::string& username,
                               const std::string& password, int expected_status) {
    if (username.empty() || username.size() > 64 || password.size() < 8 || password.size() > 128) {
        throw std::invalid_argument("Username must contain 1 to 64 bytes and password 8 to 128 bytes");
    }
    const auto body = nlohmann::json{{"username", username}, {"password", password}}.dump();
    const auto response = client_.Post(path, body, "application/json");
    if (!response) {
        throw std::runtime_error("Authentication HTTP request failed: " + httplib::to_string(response.error()));
    }
    nlohmann::json data;
    try {
        data = parse_json(response->body);
    } catch (const ProtocolError&) {
        throw std::runtime_error("Authentication service returned invalid JSON");
    }
    if (!data.is_object()) throw std::runtime_error("Authentication service returned an invalid response");
    if (response->status != expected_status) {
        const auto error = data.find("error");
        if (error != data.end() && error->is_object()) {
            const auto code = error->find("code");
            const auto message = error->find("message");
            if (code != error->end() && code->is_string() &&
                message != error->end() && message->is_string()) {
                throw std::runtime_error(code->get<std::string>() + ": " + message->get<std::string>());
            }
        }
        throw std::runtime_error("Authentication HTTP status " + std::to_string(response->status));
    }
    return data;
}

std::string AuthClient::register_user(const std::string& username, const std::string& password) {
    const auto data = post("/api/users", username, password, 201);
    const auto id = data.find("id");
    if (id == data.end() || !id->is_string() || id->get_ref<const std::string&>().empty()) {
        throw std::runtime_error("Registration response is missing the user ID");
    }
    return id->get<std::string>();
}

std::string AuthClient::login(const std::string& username, const std::string& password) {
    const auto data = post("/api/auth/token", username, password, 200);
    const auto token = data.find("access_token");
    if (token == data.end() || !token->is_string() ||
        token->get_ref<const std::string&>().empty() || token->get_ref<const std::string&>().size() > 4096) {
        throw std::runtime_error("Login response is missing a valid access token");
    }
    return token->get<std::string>();
}

} // namespace chatroom
