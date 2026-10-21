#include "session_manager.h"

namespace chatroom {

std::optional<Session> SessionManager::session(ConnectionId connection) const {
    const auto it = sessions_.find(connection);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}

std::optional<ConnectionId> SessionManager::connection_for(std::uint64_t user) const {
    const auto it = online_.find(user);
    if (it == online_.end() || sessions_.at(it->second).expired()) return std::nullopt;
    return it->second;
}

std::optional<ConnectionId> SessionManager::bind(ConnectionId connection, const Session& session) {
    disconnect(connection);
    std::optional<ConnectionId> previous;
    const auto it = online_.find(session.user_id);
    if (it != online_.end()) {
        previous = it->second;
        disconnect(*previous);
    }
    sessions_.emplace(connection, session);
    online_.emplace(session.user_id, connection);
    return previous;
}

void SessionManager::disconnect(ConnectionId connection) {
    const auto it = sessions_.find(connection);
    if (it == sessions_.end()) return;
    online_.erase(it->second.user_id);
    sessions_.erase(it);
}

} // namespace chatroom
