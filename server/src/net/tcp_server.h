#pragma once

#include "net/epoll.h"
#include "net/tcp_connection.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>

namespace chatroom {

// Single-threaded TCP Reactor. Callbacks run on the thread executing run().
// Configure handlers before run(); send(), post(), and stop() are thread-safe.
// Run once, then join the loop and all producers before destroying the server.
class TcpServer {
public:
    using MessageHandler = std::function<void(ConnectionId, std::string)>;
    using DisconnectHandler = std::function<void(ConnectionId)>;

    explicit TcpServer(std::uint16_t port = 8081, const std::string& address = "127.0.0.1",
                       std::size_t max_connections = 1024);

    void set_message_handler(MessageHandler handler);
    void set_disconnect_handler(DisconnectHandler handler);
    std::uint16_t port() const { return listener_.local_port(); }

    void run();

    // Queues a framed payload for the loop. Results for closed connections are discarded.
    void send(ConnectionId id, std::string_view payload);

    // Runs a short completion callback on the Reactor thread. Ignored after stop().
    // Captured objects must outlive run(); callbacks must not perform blocking work.
    void post(std::function<void()> callback);

    // Reactor-only: retain a half-closed connection while application work is pending.
    // resume_close() flushes queued replies before checking whether EOF can close it.
    void defer_close(ConnectionId id);
    void resume_close(ConnectionId id);

    // Reactor-only: closes a connection immediately and invokes its disconnect handler.
    void close_connection(ConnectionId id, std::string_view reason);

    // Wakes the loop and stops it without draining pending output.
    void stop();

private:
    struct PendingSend {
        ConnectionId id;
        std::string frame;
    };

    void notify();
    void accept_connections();
    void process_sends();
    void process_callbacks();
    void handle_event(ConnectionId id, std::uint32_t events);
    void update_connection(ConnectionId id);
    void cleanup();

    Socket listener_;
    Epoll epoll_;
    UniqueFd wakeup_fd_;
    std::unordered_map<ConnectionId, TcpConnection> connections_;
    std::size_t max_connections_;
    // The listener is removed from epoll while a resource-exhaustion retry is pending.
    std::optional<std::chrono::steady_clock::time_point> accept_retry_at_;
    ConnectionId next_id_ = 2;
    MessageHandler on_message_;
    DisconnectHandler on_disconnect_;

    std::atomic<bool> stopped_{false};
    std::mutex queue_mutex_;
    std::queue<PendingSend> pending_sends_;
    std::queue<std::function<void()>> pending_callbacks_;
};

} // namespace chatroom
