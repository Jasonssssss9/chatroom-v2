#pragma once

#include "net/tcp_server.h"
#include "protocol.h"
#include "thread_pool.h"

namespace chatroom {

// Serializes each connection's requests while allowing different connections to run
// in parallel. Queue state and completions belong exclusively to the Reactor thread.
// Construct before run(); destroy after the loop is stopped and joined.
class RequestDispatcher {
public:
    using Handler = std::function<nlohmann::json(ConnectionId, const Request&)>;

    RequestDispatcher(TcpServer& server, Handler handler, std::size_t worker_count = 4,
                      std::size_t max_requests_per_connection = 32,
                      std::size_t max_pending_requests = 1024);

    // Called by the Reactor's message and disconnect callbacks.
    void enqueue(ConnectionId id, std::string payload);
    void disconnect(ConnectionId id);

private:
    struct ConnectionRequests {
        std::queue<std::string> waiting;
        bool active = false;
    };

    void start_next(ConnectionId id);
    std::string process(ConnectionId id, std::string_view payload) const;
    void complete(ConnectionId id, std::string response);

    TcpServer& server_;
    Handler handler_;
    std::unordered_map<ConnectionId, ConnectionRequests> requests_;
    std::size_t max_requests_per_connection_;
    std::size_t max_pending_requests_;
    // Includes submitted tasks for disconnected clients until their completions arrive.
    std::size_t pending_requests_ = 0;
    // Destroy workers first, while the handler and server reference are still valid.
    ThreadPool workers_;
};

} // namespace chatroom
