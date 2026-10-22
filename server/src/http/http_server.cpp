#include "http/http_server.h"
#include "logger.h"
#include "protocol.h"
#include "pending_file.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <openssl/rand.h>
#include <utility>

namespace chatroom {
namespace {
using nlohmann::json;

std::unique_ptr<Database>& worker_database() {
    initialize_mysql();
    thread_local std::unique_ptr<Database> connection;
    return connection;
}

std::uint64_t require_id(const std::string& text) {
    std::uint64_t id = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || id == 0 || text.front() == '0') {
        throw AuthError(400, "invalid_request", "IDs must be positive decimal strings without leading zeros");
    }
    return id;
}

std::string storage_key() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("Cannot generate a storage key");
    constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (const auto byte : bytes) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    return key;
}

void send_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& response, int status, const std::string& code,
                 const std::string& message) {
    send_json(response, status, {{"error", {{"code", code}, {"message", message}}}});
}

bool has_content_type(const httplib::Request& request, const char* expected) {
    auto type = request.get_header_value("Content-Type");
    type = type.substr(0, type.find(';'));
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
    const auto first = type.find_first_not_of(" \t");
    if (first != std::string::npos) type.erase(0, first);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
    return type == expected;
}
} // namespace

HttpServer::HttpServer(const AuthService& auth, DatabaseConfig database,
                       const std::string& address, HttpConfig config, FileConfig files,
                       NotificationHandler notify)
    : auth_(auth), database_(std::move(database)), files_(std::move(files)), notify_(std::move(notify)) {
    if (config.worker_threads == 0) throw std::invalid_argument("HTTP worker count must be positive");
    server_.new_task_queue = [count = config.worker_threads] {
        return new httplib::ThreadPool(count, count, 128);
    };
    if (files_.directory.empty() || files_.max_size_bytes == 0 || files_.max_size_bytes > 1024ULL * 1024 * 1024) {
        throw std::invalid_argument("File storage requires a directory and a size limit between 1 byte and 1 GiB");
    }
    server_.set_payload_max_length(std::max<std::uint64_t>(4096, files_.max_size_bytes));
    server_.set_read_timeout(5);
    server_.set_write_timeout(5);
    server_.set_keep_alive_timeout(5);
    server_.set_keep_alive_max_count(10);
    server_.Post("/api/users", [this](const auto& request, auto& response, const httplib::ContentReader& reader) {
        handle(request, response, reader, true);
    });
    server_.Post("/api/auth/token", [this](const auto& request, auto& response, const httplib::ContentReader& reader) {
        handle(request, response, reader, false);
    });
    server_.Post("/api/files", [this](const auto& request, auto& response, const httplib::ContentReader& reader) {
        handle_file(request, response, &reader);
    });
    server_.Get(R"(/api/files/([0-9]+))", [this](const auto& request, auto& response) {
        handle_file(request, response);
    });
    server_.set_error_handler([](const auto& request, auto& response) {
        if (!response.body.empty()) return;
        if ((request.path == "/api/users" || request.path == "/api/auth/token" || request.path == "/api/files") &&
            request.method != "POST") {
            response.set_header("Allow", "POST");
            send_error(response, 405, "method_not_allowed", "Use POST for this endpoint");
        } else if (response.status == 413) {
            send_error(response, 413, "payload_too_large", "HTTP body exceeds the configured limit");
        } else if (response.status == 404) {
            send_error(response, 404, "not_found", "Endpoint does not exist");
        } else {
            send_error(response, response.status, "invalid_request", "HTTP request could not be processed");
        }
    });
    server_.set_exception_handler([](const auto&, auto& response, std::exception_ptr failure) {
        try { std::rethrow_exception(failure); }
        catch (const std::exception& error) { log(LogLevel::error, "http_request_failed", {{"error", error.what()}}); }
        catch (...) { log(LogLevel::error, "http_request_failed", {{"error", "Unknown exception"}}); }
        send_error(response, 500, "internal_error", "Request processing failed");
    });
    server_.set_logger([](const auto& request, const auto& response) {
        json context{{"client_ip", request.remote_addr}, {"method", request.method},
                     {"uri", request.path}, {"status", response.status}};
        if (response.status >= 400) {
            const auto body = json::parse(response.body, nullptr, false);
            if (body.is_object() && body.contains("error")) context["error"] = body["error"];
        }
        log(response.status >= 500 ? LogLevel::error : LogLevel::info, "http_request", std::move(context));
    });
    const int bound = config.port == 0 ? server_.bind_to_any_port(address)
        : (server_.bind_to_port(address, config.port) ? config.port : -1);
    if (bound < 0) throw std::runtime_error("Cannot bind the HTTP listener");
    port_ = static_cast<std::uint16_t>(bound);
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start(std::function<void()> on_failure) {
    listener_ = std::thread([this, on_failure = std::move(on_failure)] {
        try {
            server_.listen_after_bind();
            if (!stopping_) throw std::runtime_error("HTTP listener stopped unexpectedly");
        } catch (...) {
            failure_ = std::current_exception();
            server_.decommission();
            if (on_failure) on_failure();
        }
    });
    server_.wait_until_ready();
    if (!server_.is_running()) {
        stop();
        check();
    }
}

void HttpServer::stop() {
    stopping_ = true;
    server_.stop();
    if (listener_.joinable()) listener_.join();
}

void HttpServer::check() const {
    if (failure_) std::rethrow_exception(failure_);
}

void HttpServer::handle(const httplib::Request& request, httplib::Response& response,
                         const httplib::ContentReader& reader, bool registration) {
    auto& connection = worker_database();
    try {
        if (!has_content_type(request, "application/json")) {
            throw AuthError(415, "unsupported_media_type", "Content-Type must be application/json");
        }
        std::string bytes;
        bool too_large = false;
        const bool complete = reader([&](const char* data, std::size_t size) {
            if (size > 4096 - bytes.size()) { too_large = true; return false; }
            bytes.append(data, size);
            return true;
        });
        if (too_large || response.status == 413) throw AuthError(413, "payload_too_large", "HTTP body exceeds the 4 KiB limit");
        if (!complete) throw AuthError(400, "invalid_request", "HTTP body is incomplete");
        const auto body = parse_json(bytes);
        if (!body.is_object() || !body.contains("username") || !body["username"].is_string() ||
            !body.contains("password") || !body["password"].is_string()) {
            throw AuthError(400, "invalid_request", "username and password must be strings");
        }
        const auto username = body["username"].get<std::string>();
        const auto password = body["password"].get<std::string>();
        if (!connection) connection = std::make_unique<Database>(database_);
        ChatStore store(*connection);
        if (registration) {
            const auto id = auth_.register_user(store, username, password);
            send_json(response, 201, {{"id", std::to_string(id)}, {"username", username}});
        } else {
            send_json(response, 200, {{"access_token", auth_.login(store, username, password)},
                {"token_type", "Bearer"}, {"expires_in", auth_.token_lifetime_seconds()}});
        }
    } catch (const ProtocolError& error) {
        send_error(response, 400, error.code, error.what());
    } catch (const AuthError& error) {
        send_error(response, error.status, error.code, error.what());
    } catch (const DatabaseError& error) {
        connection.reset();
        log(LogLevel::error, "http_database_error", {{"code", error.code}});
        send_error(response, 500, "internal_error", "Request processing failed");
    }
}

void HttpServer::handle_file(const httplib::Request& request, httplib::Response& response,
                             const httplib::ContentReader* reader) {
    auto& connection = worker_database();
    try {
        const auto authorization = request.get_header_value("Authorization");
        if (authorization.compare(0, 7, "Bearer ") != 0) {
            throw AuthError(401, "authentication_required", "Use an Authorization: Bearer token");
        }
        if (!connection) connection = std::make_unique<Database>(database_);
        ChatStore store(*connection);
        const auto user = auth_.authenticate(store, authorization.substr(7));
        if (!reader) {
            const auto file = store.file_for_user(require_id(request.matches[1].str()), user);
            if (!file) throw AuthError(404, "file_not_found", "File is unavailable");
            const auto path = std::filesystem::path(files_.directory) / file->storage_key;
            auto [size, provider] = httplib::make_file_body(path.string());
            if (!provider) throw AuthError(404, "file_not_found", "File is unavailable");
            if (size != file->size_bytes) throw std::runtime_error("Stored file size does not match its metadata");
            response.set_header("Cache-Control", "no-store");
            response.set_header("X-Content-Type-Options", "nosniff");
            response.set_header("Content-Disposition", "attachment; filename=\"download\"; filename*=UTF-8''" +
                                httplib::encode_uri_component(file->original_name));
            response.set_content_provider(size, "application/octet-stream", std::move(provider));
            return;
        }
        if (!has_content_type(request, "application/octet-stream")) {
            throw AuthError(415, "unsupported_media_type", "Content-Type must be application/octet-stream");
        }
        if (!request.has_header("Content-Length") || request.has_header("Transfer-Encoding")) {
            throw AuthError(411, "length_required", "Uploads require Content-Length");
        }
        const auto expected_size = request.get_header_value_u64("Content-Length");
        if (expected_size > files_.max_size_bytes) throw AuthError(413, "payload_too_large", "File exceeds the configured size limit");
        const auto metadata_text = request.get_header_value("X-File-Metadata");
        if (metadata_text.size() > 4096) throw AuthError(400, "invalid_request", "File metadata exceeds 4 KiB");
        const auto metadata = parse_json(metadata_text);
        if (!metadata.is_object() || !metadata.contains("name") || !metadata["name"].is_string() ||
            !metadata.contains("recipients") || !metadata["recipients"].is_array() ||
            metadata["recipients"].empty() || metadata["recipients"].size() > 64) {
            throw AuthError(400, "invalid_request", "Provide a file name and 1 to 64 recipient ID strings");
        }
        const auto name = metadata["name"].get<std::string>();
        if (name.empty() || name.size() > 255 || name == "." || name == ".." ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '\\'; })) {
            throw AuthError(400, "invalid_request", "File name must contain 1 to 255 bytes without paths or control characters");
        }
        std::vector<std::uint64_t> recipients;
        for (const auto& value : metadata["recipients"]) {
            if (!value.is_string()) throw AuthError(400, "invalid_request", "Recipients must be user ID strings");
            recipients.push_back(require_id(value.get<std::string>()));
        }
        std::sort(recipients.begin(), recipients.end());
        recipients.erase(std::unique(recipients.begin(), recipients.end()), recipients.end());
        if (store.existing_user_ids(recipients) != recipients) throw AuthError(400, "invalid_recipient", "A recipient does not exist");

        std::filesystem::create_directories(files_.directory);
        const auto key = storage_key();
        PendingFile file(std::filesystem::path(files_.directory) / key);
        const bool complete = (*reader)([&](const char* data, std::size_t size) {
            if (size > files_.max_size_bytes - file.size()) return false;
            file.append(std::string_view(data, size));
            return true;
        });
        if (!complete || file.size() != expected_size) throw AuthError(400, "invalid_request", "File upload is incomplete");
        file.finish();
        const auto saved = store.create_file(user, key, name, "application/octet-stream", file.size(), recipients);
        file.keep();
        // The database commit owns delivery now, even if the HTTP response is lost.
        if (notify_) notify_({recipients, make_file_notification(saved.message_id,
            FileInfo{saved.id, user, key, name, "application/octet-stream", file.size()}).dump()});
        response.set_header("Location", "/api/files/" + std::to_string(saved.id));
        send_json(response, 201, {{"file_id", std::to_string(saved.id)}, {"message_id", std::to_string(saved.message_id)},
                                 {"name", name}, {"size_bytes", file.size()}});
    } catch (const ProtocolError& error) {
        send_error(response, 400, error.code, error.what());
    } catch (const AuthError& error) {
        if (error.status == 401) response.set_header("WWW-Authenticate", "Bearer");
        send_error(response, error.status, error.code, error.what());
    } catch (const DatabaseError& error) {
        connection.reset();
        log(LogLevel::error, "http_database_error", {{"code", error.code}});
        send_error(response, 500, "internal_error", "Request processing failed");
    }
}

} // namespace chatroom
