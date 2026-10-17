#include "request_dispatcher.h"
#include "framing.h"
#include "logger.h"

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
} // namespace

RequestDispatcher::RequestDispatcher(TcpServer& server, Handler handler, std::size_t worker_count)
    : server_(server), handler_(std::move(handler)), workers_(worker_count) {}

void RequestDispatcher::enqueue(ConnectionId id, std::string payload) {
    auto& queue = requests_[id];
    const bool idle = queue.empty();
    queue.push(std::move(payload));
    if (idle) {
        server_.defer_close(id);
        start_next(id);
    }
}

void RequestDispatcher::disconnect(ConnectionId id) {
    requests_.erase(id);
}

void RequestDispatcher::start_next(ConnectionId id) {
    auto payload = std::move(requests_.at(id).front());
    workers_.submit([this, id, payload = std::move(payload)] {
        auto response = process(id, payload);
        server_.post([this, id, response = std::move(response)]() mutable {
            complete(id, std::move(response));
        });
    });
}

std::string RequestDispatcher::process(ConnectionId id, std::string_view payload) const {
    Request request;
    try {
        request = parse_request(payload);
        auto response = handler_(id, request);
        auto serialized = response.dump();
        // Apply the wire limit on a worker before the result reaches the Reactor.
        if (serialized.size() > kMaxFrameSize) {
            response = make_error_response(request, "response_too_large",
                                           "Response exceeds the 64 KiB limit");
            serialized = response.dump();
        }
        if (!response.value("ok", true)) {
            const auto& error = response.at("error");
            log_request_error(id, request, error.at("code").get<std::string>(),
                              error.at("message").get<std::string>());
        } else {
            log(LogLevel::debug, "request_completed", request_context(id, request));
        }
        return serialized;
    } catch (const ProtocolError& error) {
        log_request_error(id, error.request, error.code, error.what());
        return make_error_response(error.request, error.code, error.what()).dump();
    } catch (const std::exception& error) {
        log_request_error(id, request, "internal_error", error.what());
    } catch (...) {
        log_request_error(id, request, "internal_error", "Unknown exception");
    }
    return make_error_response(request, "internal_error", "Request processing failed").dump();
}

void RequestDispatcher::complete(ConnectionId id, std::string response) {
    auto it = requests_.find(id);
    if (it == requests_.end()) {
        return;
    }
    server_.send(id, response);
    it->second.pop();
    if (it->second.empty()) {
        requests_.erase(it);
        server_.resume_close(id);
    } else {
        start_next(id);
    }
}

} // namespace chatroom
