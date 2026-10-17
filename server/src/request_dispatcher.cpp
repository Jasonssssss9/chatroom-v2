#include "request_dispatcher.h"

#include <iostream>
#include <utility>

namespace chatroom {

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
        return handler_(id, request).dump();
    } catch (const ProtocolError& error) {
        return make_error_response(error.request, error.code, error.what()).dump();
    } catch (const std::exception& error) {
        std::cerr << "Request on connection " << id << " failed: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "Request on connection " << id << " failed with an unknown exception\n";
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
