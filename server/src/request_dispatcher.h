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

    RequestDispatcher(TcpServer& server, Handler handler, std::size_t worker_count = 4);

    // Called by the Reactor's message and disconnect callbacks.
    void enqueue(ConnectionId id, std::string payload);
    void disconnect(ConnectionId id);

private:
    void start_next(ConnectionId id);
    std::string process(ConnectionId id, std::string_view payload) const;
    void complete(ConnectionId id, std::string response);

    TcpServer& server_;
    Handler handler_;
    // The front entry marks the active request, even after its payload is moved out.
    std::unordered_map<ConnectionId, std::queue<std::string>> requests_;
    // Destroy workers first, while the handler and server reference are still valid.
    ThreadPool workers_;
};

} // namespace chatroom
