#pragma once

#include "net/tcp_server.h"
#include "protocol.h"
#include "request_handler.h"
#include "session_manager.h"
#include "thread_pool.h"

#include <unordered_set>

namespace chatroom {

// Serializes each connection's requests while allowing different connections to run
// in parallel. Queue state and completions belong exclusively to the Reactor thread.
// Construct before run(); destroy after the loop is stopped and joined.
class RequestDispatcher {
public:
    using Handler = std::function<RequestResult(ConnectionId, const Request&, std::optional<Session>)>;
    using ReplayHandler = std::function<std::vector<MessageDelivery>(std::uint64_t)>;

    RequestDispatcher(TcpServer& server, Handler handler, std::size_t worker_count = 4,
                      std::size_t max_requests_per_connection = 32,
                      std::size_t max_pending_requests = 1024, ReplayHandler replay = {});

    // Called by the Reactor's message and disconnect callbacks.
    void enqueue(ConnectionId id, std::string payload);
    void disconnect(ConnectionId id);
    // Reactor-only access for online routing.
    const SessionManager& sessions() const { return sessions_; }

private:
    struct ConnectionRequests {
        std::queue<std::string> waiting;
        bool active = false;
        bool replay_requested = false;
        // Retained while idle; the last successful ACK allows another query.
        std::unordered_set<std::uint64_t> replay_in_flight;
    };

    struct Completion {
        std::string response;
        std::optional<Session> session;
        // Correlation fields only; request data and tokens remain on the worker.
        Request context;
        std::optional<UserNotification> notification = std::nullopt;
        std::optional<std::uint64_t> acknowledged_message = std::nullopt;
    };

    void start_next(ConnectionId id);
    void advance(ConnectionId id);
    void start_replay(ConnectionId id, std::uint64_t user);
    void complete_replay(ConnectionId id, std::vector<MessageDelivery> deliveries,
                         std::optional<std::string> error);
    Completion process(ConnectionId id, std::string_view payload, std::optional<Session> session) const;
    void complete(ConnectionId id, Completion result);

    TcpServer& server_;
    Handler handler_;
    ReplayHandler replay_;
    SessionManager sessions_;
    std::unordered_map<ConnectionId, ConnectionRequests> requests_;
    std::size_t max_requests_per_connection_;
    std::size_t max_pending_requests_;
    // Includes submitted tasks for disconnected clients until their completions arrive.
    std::size_t pending_requests_ = 0;
    // Destroy workers first, while the handler and server reference are still valid.
    ThreadPool workers_;
};

} // namespace chatroom
