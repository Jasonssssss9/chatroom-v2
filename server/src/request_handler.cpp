#include "request_handler.h"
#include "db/chat_store.h"

#include <utility>

namespace chatroom {

RequestHandler::RequestHandler(const TokenService& tokens, DatabaseConfig database)
    : tokens_(tokens), database_(std::move(database)) {}

RequestResult RequestHandler::handle(std::string_view payload, std::optional<Session> session) const {
    try {
        return handle(parse_request(payload), session);
    } catch (const ProtocolError& error) {
        return {make_error_response(error.request, error.code, error.what())};
    }
}

RequestResult RequestHandler::handle(const Request& request, std::optional<Session> session) const {
    if (request.action == "ping") {
        return {make_response(request, {{"pong", true}})};
    }
    if (request.action == "authenticate") {
        const auto token = request.data.find("token");
        if (token == request.data.end() || !token->is_string() ||
            token->get_ref<const std::string&>().empty() || token->get_ref<const std::string&>().size() > 4096) {
            return {make_error_response(request, "invalid_request", "token must contain 1 to 4096 bytes")};
        }
        const auto identity = tokens_.verify(token->get_ref<const std::string&>());
        if (!identity) {
            return {make_error_response(request, "invalid_token", "Token is invalid or expired")};
        }
        if (session && session->user_id != identity->user_id) {
            return {make_error_response(request, "already_authenticated", "Reconnect to change users")};
        }
        // Each pool thread owns its connection; initialize MySQL before declaring it.
        initialize_mysql();
        thread_local std::unique_ptr<Database> connection;
        try {
            if (!connection) connection = std::make_unique<Database>(database_);
            ChatStore store(*connection);
            if (!store.user_by_id(identity->user_id)) {
                return {make_error_response(request, "invalid_token", "Token is invalid or expired")};
            }
        } catch (const DatabaseError&) {
            connection.reset();
            throw;
        }
        return {make_response(request, {{"user_id", std::to_string(identity->user_id)}}), identity};
    }
    if (!session || session->expired()) {
        return {make_error_response(request, "authentication_required", "Authenticate before using this action")};
    }
    return {make_error_response(request, "unknown_action", "Action is not supported")};
}

} // namespace chatroom
