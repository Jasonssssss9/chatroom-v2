#pragma once

#include "protocol.h"
#include "auth/token_service.h"
#include "config.h"

#include <vector>

namespace chatroom {

class Database;

// One serialized notification, routed to users by the Reactor after persistence.
struct UserNotification {
    std::vector<std::uint64_t> recipients;
    std::string payload;
};

struct RequestResult {
    nlohmann::json response;
    std::optional<Session> authenticated_session = std::nullopt;
    std::optional<UserNotification> notification = std::nullopt;
};

// Worker-side application dispatch. Session changes are returned to the Reactor.
class RequestHandler {
public:
    RequestHandler(const TokenService& tokens, DatabaseConfig database);
    RequestResult handle(std::string_view payload, std::optional<Session> session = {}) const;
    RequestResult handle(const Request& request, std::optional<Session> session = {}) const;

private:
    RequestResult authenticate(const Request& request, std::optional<Session> session) const;
    RequestResult send_message(const Request& request, std::uint64_t sender) const;
    Database& database() const;

    const TokenService& tokens_;
    DatabaseConfig database_;
};

} // namespace chatroom
