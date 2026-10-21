#pragma once

#include "protocol.h"
#include "auth/token_service.h"
#include "config.h"

namespace chatroom {

struct RequestResult {
    nlohmann::json response;
    std::optional<Session> authenticated_session = std::nullopt;
};

// Worker-side application dispatch. Session changes are returned to the Reactor.
class RequestHandler {
public:
    RequestHandler(const TokenService& tokens, DatabaseConfig database);
    RequestResult handle(std::string_view payload, std::optional<Session> session = {}) const;
    RequestResult handle(const Request& request, std::optional<Session> session = {}) const;

private:
    const TokenService& tokens_;
    DatabaseConfig database_;
};

} // namespace chatroom
