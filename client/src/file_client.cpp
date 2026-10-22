#include "file_client.h"
#include "pending_file.h"
#include "protocol.h"

#include <stdexcept>

namespace chatroom {
namespace {
void check_response(const httplib::Result& response, const std::string& body, int expected) {
    if (!response) throw std::runtime_error("File HTTP request failed: " + httplib::to_string(response.error()));
    if (response->status == expected) return;
    try {
        const auto data = parse_json(body);
        const auto& error = data.at("error");
        throw std::runtime_error(error.at("code").get<std::string>() + ": " + error.at("message").get<std::string>() +
                                 (response->status == 401 ? ". Use /login to authenticate again." : ""));
    } catch (const ProtocolError&) {
        throw std::runtime_error("File HTTP status " + std::to_string(response->status));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("File HTTP status " + std::to_string(response->status));
    }
}
} // namespace

FileClient::FileClient(const std::string& address, std::uint16_t port) : client_(address, port) {
    client_.set_connection_timeout(5);
    client_.set_read_timeout(30);
    client_.set_write_timeout(30);
    client_.set_max_timeout(120000);
    client_.set_payload_max_length(1024ULL * 1024 * 1024);
}

nlohmann::json FileClient::upload(const std::string& token, const std::filesystem::path& path,
                                  nlohmann::json recipients) {
    if (token.empty()) throw std::runtime_error("Use /login before transferring files");
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Upload path must be a regular file");
    auto [size, provider] = httplib::make_file_body(path.string());
    if (!provider) throw std::runtime_error("Cannot open the upload file");
    const auto metadata = nlohmann::json{{"name", path.filename().string()}, {"recipients", std::move(recipients)}};
    const httplib::Headers headers{{"Authorization", "Bearer " + token},
                                   {"X-File-Metadata", metadata.dump(-1, ' ', true)}};
    std::string body;
    const auto response = client_.Post("/api/files", headers, size, std::move(provider),
        "application/octet-stream", [&](const char* data, std::size_t length) {
            if (length > 8192 - body.size()) return false;
            body.append(data, length);
            return true;
        });
    check_response(response, body, 201);
    const auto result = parse_json(body);
    if (!result.is_object() || !result.contains("file_id") || !result["file_id"].is_string() ||
        !result.contains("message_id") || !result["message_id"].is_string()) {
        throw std::runtime_error("Upload response is missing file or message IDs");
    }
    return result;
}

void FileClient::download(const std::string& token, const std::string& id, const std::filesystem::path& path) {
    if (token.empty()) throw std::runtime_error("Use /login before transferring files");
    PendingFile file(path);
    bool success = false;
    std::string error_body;
    const auto response = client_.Get("/api/files/" + httplib::encode_uri_component(id),
        {{"Authorization", "Bearer " + token}},
        [&](const httplib::Response& result) { success = result.status == 200; return true; },
        [&](const char* data, std::size_t length) {
            if (success) file.append(std::string_view(data, length));
            else {
                if (length > 8192 - error_body.size()) return false;
                error_body.append(data, length);
            }
            return true;
        });
    check_response(response, error_body, 200);
    file.finish();
    file.keep();
}

} // namespace chatroom
