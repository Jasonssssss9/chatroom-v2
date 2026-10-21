#include "request_handler.h"
#include "db/chat_store.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace chatroom {
namespace {
// Initialize thread state first, so the connection closes before MySQL thread cleanup.
std::unique_ptr<Database>& worker_database() {
    initialize_mysql();
    thread_local std::unique_ptr<Database> connection;
    return connection;
}

std::optional<std::uint64_t> parse_user_id(const nlohmann::json& value) {
    if (!value.is_string()) return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t id = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        id == 0 || text.front() == '0') return std::nullopt;
    return id;
}
} // namespace

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
    try {
        if (request.action == "authenticate") return authenticate(request, session);
        if (!session || session->expired()) {
            return {make_error_response(request, "authentication_required", "Authenticate before using this action")};
        }
        if (request.action == "send_message") return send_message(request, session->user_id);
        return {make_error_response(request, "unknown_action", "Action is not supported")};
    } catch (const DatabaseError&) {
        worker_database().reset();
        throw;
    }
}

Database& RequestHandler::database() const {
    auto& connection = worker_database();
    if (!connection) connection = std::make_unique<Database>(database_);
    return *connection;
}

RequestResult RequestHandler::authenticate(const Request& request, std::optional<Session> session) const {
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
    ChatStore store(database());
    if (!store.user_by_id(identity->user_id)) {
        return {make_error_response(request, "invalid_token", "Token is invalid or expired")};
    }
    return {make_response(request, {{"user_id", std::to_string(identity->user_id)}}), identity};
}

RequestResult RequestHandler::send_message(const Request& request, std::uint64_t sender) const {
    const auto& data = request.data;
    const auto client_id = data.find("client_message_id");
    const auto text = data.find("text");
    const auto targets = data.find("recipients");
    if (client_id == data.end() || !client_id->is_string() ||
        client_id->get_ref<const std::string&>().empty() || client_id->get_ref<const std::string&>().size() > 64) {
        return {make_error_response(request, "invalid_request", "client_message_id must contain 1 to 64 bytes")};
    }
    // This text limit keeps notifications within the frame limit even after JSON escaping.
    if (text == data.end() || !text->is_string() ||
        text->get_ref<const std::string&>().empty() || text->get_ref<const std::string&>().size() > 4096) {
        return {make_error_response(request, "invalid_request", "text must contain 1 to 4096 bytes")};
    }
    if (targets == data.end() || !targets->is_array() || targets->empty() || targets->size() > 64) {
        return {make_error_response(request, "invalid_request", "recipients must contain 1 to 64 user ID strings")};
    }
    std::vector<std::uint64_t> recipients;
    for (const auto& value : *targets) {
        const auto id = parse_user_id(value);
        if (!id) {
            return {make_error_response(request, "invalid_request", "recipients must contain positive user ID strings")};
        }
        recipients.push_back(*id);
    }
    std::sort(recipients.begin(), recipients.end());
    recipients.erase(std::unique(recipients.begin(), recipients.end()), recipients.end());

    ChatStore store(database());
    for (const auto recipient : recipients) {
        if (!store.user_by_id(recipient)) {
            return {make_error_response(request, "invalid_recipient", "A recipient does not exist")};
        }
    }
    std::uint64_t message;
    try {
        message = store.save_direct_message(sender, client_id->get_ref<const std::string&>(),
                                             text->get_ref<const std::string&>(), recipients);
    } catch (const std::invalid_argument&) {
        return {make_error_response(request, "message_conflict", "Client message ID belongs to a different message")};
    }
    const auto notification = make_notification("message",
        {{"message_id", std::to_string(message)}, {"sender_id", std::to_string(sender)},
         {"client_message_id", *client_id}, {"text", *text}});
    return {make_response(request, {{"message_id", std::to_string(message)}}), std::nullopt,
            UserNotification{std::move(recipients), notification.dump()}};
}

} // namespace chatroom
