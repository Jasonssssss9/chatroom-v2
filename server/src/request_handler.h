#pragma once

#include "protocol.h"
#include "auth/token_service.h"
#include "config.h"
#include "notification.h"

#include <vector>

namespace chatroom {

class Database;

inline constexpr std::size_t kReplayBatchCount = 16;
inline constexpr std::size_t kReplayBatchBytes = 128 * 1024;

struct MessageDelivery {
    std::uint64_t message_id;
    std::string payload;
};

struct RequestResult {
    nlohmann::json response;
    std::optional<Session> authenticated_session = std::nullopt;
    std::optional<UserNotification> notification = std::nullopt;
    std::optional<std::uint64_t> acknowledged_message = std::nullopt;
};

// Worker-side application dispatch. Session changes are returned to the Reactor.
class RequestHandler {
public:
    RequestHandler(const TokenService& tokens, DatabaseConfig database);
    RequestResult handle(std::string_view payload, std::optional<Session> session = {}) const;
    RequestResult handle(const Request& request, std::optional<Session> session = {}) const;
    // Called on a worker only after the Reactor has bound the authenticated session.
    std::vector<MessageDelivery> pending_messages(std::uint64_t recipient) const;

private:
    RequestResult authenticate(const Request& request, std::optional<Session> session) const;
    RequestResult send_message(const Request& request, std::uint64_t sender) const;
    RequestResult create_group(const Request& request, std::uint64_t owner) const;
    RequestResult group_members(const Request& request, std::uint64_t user) const;
    RequestResult list_groups(const Request& request, std::uint64_t user) const;
    RequestResult acknowledge_message(const Request& request, std::uint64_t recipient) const;
    Database& database() const;

    const TokenService& tokens_;
    DatabaseConfig database_;
};

} // namespace chatroom
