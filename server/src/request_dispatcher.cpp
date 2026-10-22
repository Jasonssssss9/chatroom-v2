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
                                     std::size_t max_pending_requests, ReplayHandler replay)
    : server_(server), handler_(std::move(handler)), replay_(std::move(replay)),
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
        advance(id);
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
        std::optional<std::uint64_t> acknowledged;
        try {
            request = parse_request(payload);
            auto result = handler_(id, request, session);
            response = std::move(result.response);
            authenticated = result.authenticated_session;
            notification = std::move(result.notification);
            acknowledged = result.acknowledged_message;
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
            acknowledged.reset();
            const auto& error = response.at("error");
            log_request_error(id, request, error.at("code").get<std::string>(),
                              error.at("message").get<std::string>());
        } else {
            log(LogLevel::debug, "request_completed", request_context(id, request));
        }
        return {std::move(serialized), authenticated, {request.request_id, request.action},
                std::move(notification), acknowledged};
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
            it->second.replay_in_flight.clear();
            it->second.replay_requested = static_cast<bool>(replay_);
        }
    }
    if (it != requests_.end()) {
        it->second.active = false;
        server_.send(id, result.response);
        auto& in_flight = it->second.replay_in_flight;
        if (result.acknowledged_message && in_flight.erase(*result.acknowledged_message) != 0 &&
            in_flight.empty()) {
            it->second.replay_requested = true;
        }
    }
    // A committed message remains deliverable even if its sender disconnected.
    if (result.notification) notify(*result.notification);
    if (it == requests_.end()) return;
    advance(id);
}

void RequestDispatcher::notify(const UserNotification& notification) {
    for (const auto recipient : notification.recipients) {
        if (const auto connection = sessions_.connection_for(recipient)) {
            server_.send(*connection, notification.payload);
        }
    }
}

void RequestDispatcher::advance(ConnectionId id) {
    auto& state = requests_.at(id);
    const auto session = sessions_.session(id);
    if (state.replay_requested && state.replay_in_flight.empty() && session && !session->expired() &&
        state.waiting.size() < max_requests_per_connection_ && pending_requests_ < max_pending_requests_) {
        state.replay_requested = false;
        start_replay(id, session->user_id);
    } else if (!state.waiting.empty()) {
        start_next(id);
    } else {
        server_.resume_close(id);
    }
}

void RequestDispatcher::start_replay(ConnectionId id, std::uint64_t user) {
    requests_.at(id).active = true;
    ++pending_requests_;
    workers_.submit([this, id, user] {
        std::vector<MessageDelivery> deliveries;
        std::optional<std::string> error;
        try {
            deliveries = replay_(user);
            std::size_t bytes = 0;
            for (const auto& delivery : deliveries) {
                if (delivery.payload.size() > kMaxFrameSize) {
                    throw std::length_error("Replay notification exceeds the frame limit");
                }
                bytes += delivery.payload.size() + 4;
            }
            if (deliveries.size() > kReplayBatchCount || bytes > kReplayBatchBytes) {
                throw std::length_error("Replay exceeds the batch limit");
            }
        } catch (const std::exception& failure) {
            error = failure.what();
        } catch (...) {
            error = "Unknown exception";
        }
        server_.post([this, id, deliveries = std::move(deliveries), error = std::move(error)]() mutable {
            complete_replay(id, std::move(deliveries), std::move(error));
        });
    });
}

void RequestDispatcher::complete_replay(ConnectionId id, std::vector<MessageDelivery> deliveries,
                                        std::optional<std::string> error) {
    --pending_requests_;
    const auto it = requests_.find(id);
    if (it == requests_.end()) return;
    it->second.active = false;
    if (error) {
        log(LogLevel::error, "message_replay_failed", {{"connection_id", id}, {"error", *error}});
        server_.close_connection(id, "message_replay_failed");
        return;
    }
    const auto session = sessions_.session(id);
    if (session && !session->expired()) {
        for (const auto& delivery : deliveries) {
            it->second.replay_in_flight.insert(delivery.message_id);
            server_.send(id, delivery.payload);
        }
    }
    advance(id);
}

} // namespace chatroom
