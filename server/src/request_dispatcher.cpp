#include "request_dispatcher.h"
#include "framing.h"
#include "logger.h"

#include <stdexcept>
#include <utility>

namespace chatroom {
namespace {
// Correlate failures without recording request bodies or future credentials.
nlohmann::json request_context(ConnectionId id, const Request& request) {
    return {{"connection_id", id},
            {"request_id", request.request_id.empty() ? nlohmann::json(nullptr)
                                                       : nlohmann::json(request.request_id)},
            {"action", request.action.empty() ? nlohmann::json(nullptr)
                                               : nlohmann::json(request.action)}};
}

void log_request_error(ConnectionId id, const Request& request,
                       std::string_view code, std::string_view message) {
    auto context = request_context(id, request);
    context["code"] = code;
    context["error"] = message;
    const auto level = code == "internal_error" ? LogLevel::error : LogLevel::warn;
    log(level, "request_error", std::move(context));
}

// Replace oversized responses before logging or posting them to the Reactor.
std::string serialize_response(const Request& request, nlohmann::json& response) {
    auto serialized = response.dump();
    if (serialized.size() > kMaxFrameSize) {
        response = make_error_response(request, "response_too_large",
                                       "Response exceeds the 64 KiB limit");
        serialized = response.dump();
    }
    return serialized;
}
} // namespace

RequestDispatcher::RequestDispatcher(TcpServer& server, Handler handler, std::size_t worker_count,
                                     std::size_t max_requests_per_connection,
                                     std::size_t max_pending_requests)
    : server_(server), handler_(std::move(handler)),
      max_requests_per_connection_(max_requests_per_connection),
      max_pending_requests_(max_pending_requests), workers_(worker_count) {
    if (max_requests_per_connection_ == 0 || max_pending_requests_ == 0) {
        throw std::invalid_argument("Request limits must be positive");
    }
}

void RequestDispatcher::enqueue(ConnectionId id, std::string payload) {
    const auto it = requests_.find(id);
    const auto connection_requests = it == requests_.end()
        ? 0 : it->second.waiting.size() + (it->second.active ? 1 : 0);
    if (connection_requests >= max_requests_per_connection_ ||
        pending_requests_ >= max_pending_requests_) {
        log(LogLevel::warn, "request_rejected",
            {{"connection_id", id},
             {"reason", connection_requests >= max_requests_per_connection_
                            ? "connection_request_limit" : "global_request_limit"},
             {"connection_requests", connection_requests}, {"pending_requests", pending_requests_},
             {"max_requests_per_connection", max_requests_per_connection_},
             {"max_pending_requests", max_pending_requests_}});
        server_.close_connection(id, "request_limit");
        return;
    }
    auto& state = requests_[id];
    state.waiting.push(std::move(payload));
    ++pending_requests_;
    if (!state.active) {
        server_.defer_close(id);
        start_next(id);
    }
}

void RequestDispatcher::disconnect(ConnectionId id) {
    sessions_.disconnect(id);
    const auto it = requests_.find(id);
    if (it != requests_.end()) {
        // A submitted task keeps its slot even after the connection disappears.
        pending_requests_ -= it->second.waiting.size();
        requests_.erase(it);
    }
}

void RequestDispatcher::start_next(ConnectionId id) {
    auto& state = requests_.at(id);
    auto payload = std::move(state.waiting.front());
    workers_.submit([this, id, payload = std::move(payload), session = sessions_.session(id)] {
        auto result = process(id, payload, session);
        server_.post([this, id, result = std::move(result)]() mutable {
            complete(id, std::move(result));
        });
    });
    state.waiting.pop();
    state.active = true;
}

RequestDispatcher::Completion RequestDispatcher::process(
    ConnectionId id, std::string_view payload, std::optional<Session> session) const {
    Request request;
    try {
        nlohmann::json response;
        std::optional<Session> authenticated;
        std::optional<UserNotification> notification;
        try {
            request = parse_request(payload);
            auto result = handler_(id, request, session);
            response = std::move(result.response);
            authenticated = result.authenticated_session;
            notification = std::move(result.notification);
        } catch (const ProtocolError& error) {
            request = error.request;
            response = make_error_response(request, error.code, error.what());
        }
        if (notification && notification->payload.size() > kMaxFrameSize) {
            response = make_error_response(request, "response_too_large", "Notification exceeds the 64 KiB limit");
        }
        auto serialized = serialize_response(request, response);
        if (!response.value("ok", true)) {
            authenticated.reset();
            notification.reset();
            const auto& error = response.at("error");
            log_request_error(id, request, error.at("code").get<std::string>(),
                              error.at("message").get<std::string>());
        } else {
            log(LogLevel::debug, "request_completed", request_context(id, request));
        }
        return {std::move(serialized), authenticated, {request.request_id, request.action}, std::move(notification)};
    } catch (const std::exception& error) {
        log_request_error(id, request, "internal_error", error.what());
    } catch (...) {
        log_request_error(id, request, "internal_error", "Unknown exception");
    }
    auto response = make_error_response(request, "internal_error", "Request processing failed");
    return {serialize_response(request, response), std::nullopt, {request.request_id, request.action}};
}

void RequestDispatcher::complete(ConnectionId id, Completion result) {
    --pending_requests_;
    auto it = requests_.find(id);
    if (it != requests_.end() && result.session) {
        if (result.session->expired()) {
            // Verification or the completion queue may have outlasted the token.
            result.response = make_error_response(result.context, "invalid_token", "Token is invalid or expired").dump();
            log_request_error(id, result.context, "invalid_token", "Token is invalid or expired");
        } else {
            const auto previous = sessions_.bind(id, *result.session);
            if (previous) {
                log(LogLevel::info, "session_replaced",
                    {{"user_id", result.session->user_id}, {"connection_id", id},
                     {"previous_connection_id", *previous}});
                server_.close_connection(*previous, "session_replaced");
            }
            log(LogLevel::info, "session_authenticated",
                {{"connection_id", id}, {"user_id", result.session->user_id}});
        }
    }
    if (it != requests_.end()) {
        it->second.active = false;
        server_.send(id, result.response);
    }
    // A committed message remains deliverable even if its sender disconnected.
    if (result.notification) {
        for (const auto recipient : result.notification->recipients) {
            if (const auto connection = sessions_.connection_for(recipient)) {
                server_.send(*connection, result.notification->payload);
            }
        }
    }
    if (it == requests_.end()) return;
    if (it->second.waiting.empty()) {
        requests_.erase(it);
        server_.resume_close(id);
    } else {
        start_next(id);
    }
}

} // namespace chatroom
