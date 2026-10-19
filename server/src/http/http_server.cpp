#include "http/http_server.h"
#include "logger.h"
#include "protocol.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace chatroom {
namespace {
using nlohmann::json;

void send_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& response, int status, const std::string& code,
                 const std::string& message) {
    send_json(response, status, {{"error", {{"code", code}, {"message", message}}}});
}

bool is_json_content(const httplib::Request& request) {
    auto type = request.get_header_value("Content-Type");
    type = type.substr(0, type.find(';'));
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
    const auto first = type.find_first_not_of(" \t");
    if (first != std::string::npos) type.erase(0, first);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
    return type == "application/json";
}
} // namespace

HttpServer::HttpServer(const AuthService& auth, DatabaseConfig database,
                       const std::string& address, HttpConfig config)
    : auth_(auth), database_(std::move(database)) {
    if (config.worker_threads == 0) throw std::invalid_argument("HTTP worker count must be positive");
    server_.new_task_queue = [count = config.worker_threads] {
        return new httplib::ThreadPool(count, count, 128);
    };
    server_.set_payload_max_length(4096);
    server_.set_read_timeout(5);
    server_.set_write_timeout(5);
    server_.set_keep_alive_timeout(5);
    server_.set_keep_alive_max_count(10);
    server_.Post("/api/users", [this](const auto& request, auto& response) {
        handle(request, response, true);
    });
    server_.Post("/api/auth/token", [this](const auto& request, auto& response) {
        handle(request, response, false);
    });
    server_.set_error_handler([](const auto& request, auto& response) {
        if (!response.body.empty()) return;
        if ((request.path == "/api/users" || request.path == "/api/auth/token") &&
            request.method != "POST") {
            response.set_header("Allow", "POST");
            send_error(response, 405, "method_not_allowed", "Use POST for this endpoint");
        } else if (response.status == 413) {
            send_error(response, 413, "payload_too_large", "HTTP body exceeds the 4 KiB limit");
        } else if (response.status == 404) {
            send_error(response, 404, "not_found", "Endpoint does not exist");
        } else {
            send_error(response, response.status, "invalid_request", "HTTP request could not be processed");
        }
    });
    server_.set_exception_handler([](const auto&, auto& response, std::exception_ptr) {
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
                         bool registration) {
    // Initialize thread state first so the connection is destroyed before MySQL thread cleanup.
    initialize_mysql();
    // Each HTTP pool thread belongs to one server and reuses only its own connection.
    thread_local std::unique_ptr<Database> connection;
    try {
        if (!is_json_content(request)) {
            throw AuthError(415, "unsupported_media_type", "Content-Type must be application/json");
        }
        const auto body = parse_json(request.body);
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

} // namespace chatroom
