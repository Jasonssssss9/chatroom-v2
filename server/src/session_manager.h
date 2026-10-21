#pragma once

#include "auth/token_service.h"
#include "net/tcp_connection.h"

#include <unordered_map>

namespace chatroom {

// Reactor-only online state. Workers receive copies, never references to these maps.
class SessionManager {
public:
    std::optional<Session> session(ConnectionId connection) const;
    // Expired sessions are unavailable for online delivery.
    std::optional<ConnectionId> connection_for(std::uint64_t user) const;
    // Replaces the user's previous binding and returns the connection to close.
    std::optional<ConnectionId> bind(ConnectionId connection, const Session& session);
    void disconnect(ConnectionId connection);

private:
    std::unordered_map<ConnectionId, Session> sessions_;
    std::unordered_map<std::uint64_t, ConnectionId> online_;
};

} // namespace chatroom
