#include "request_handler.h"
#include "db/chat_store.h"
#include "framing.h"

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

std::optional<std::uint64_t> parse_id(const nlohmann::json& value) {
    if (!value.is_string()) return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t id = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        id == 0 || text.front() == '0') return std::nullopt;
    return id;
}

const std::string& require_string(const Request& request, const char* name, std::size_t maximum) {
    const auto value = request.data.find(name);
    if (value == request.data.end() || !value->is_string() ||
        value->get_ref<const std::string&>().empty() || value->get_ref<const std::string&>().size() > maximum) {
        throw ProtocolError("invalid_request", std::string(name) + " must contain 1 to " +
                            std::to_string(maximum) + " bytes", request);
    }
    return value->get_ref<const std::string&>();
}

std::uint64_t require_id(const Request& request, const char* name) {
    const auto value = request.data.find(name);
    const auto id = value == request.data.end() ? std::nullopt : parse_id(*value);
    if (!id) throw ProtocolError("invalid_request", std::string(name) + " must be a positive ID string", request);
    return *id;
}

std::vector<std::uint64_t> parse_ids(const Request& request, const char* name, bool optional = false) {
    const auto values = request.data.find(name);
    if (optional && values == request.data.end()) return {};
    if (values == request.data.end() || !values->is_array() ||
        (!optional && values->empty()) || values->size() > 64) {
        throw ProtocolError("invalid_request", std::string(name) + " must contain " +
                            (optional ? "0" : "1") + " to 64 user ID strings", request);
    }
    std::vector<std::uint64_t> ids;
    for (const auto& value : *values) {
        const auto id = parse_id(value);
        if (!id) throw ProtocolError("invalid_request", std::string(name) + " must contain positive user ID strings", request);
        ids.push_back(*id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

nlohmann::json id_strings(const std::vector<std::uint64_t>& ids) {
    auto values = nlohmann::json::array();
    for (const auto id : ids) values.push_back(std::to_string(id));
    return values;
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
        if (request.action == "send_message" || request.action == "send_group_message") {
            return send_message(request, session->user_id);
        }
        if (request.action == "create_group") return create_group(request, session->user_id);
        if (request.action == "get_group_members") return group_members(request, session->user_id);
        if (request.action == "ack_message") return acknowledge_message(request, session->user_id);
        return {make_error_response(request, "unknown_action", "Action is not supported")};
    } catch (const ProtocolError& error) {
        return {make_error_response(error.request, error.code, error.what())};
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
    const auto& client_id = require_string(request, "client_message_id", 64);
    // This text limit keeps notifications within the frame limit even after JSON escaping.
    const auto& text = require_string(request, "text", 4096);
    const std::optional<std::uint64_t> group = request.action == "send_group_message"
        ? std::optional<std::uint64_t>(require_id(request, "group_id")) : std::nullopt;
    auto recipients = group ? std::vector<std::uint64_t>{} : parse_ids(request, "recipients");

    ChatStore store(database());
    std::uint64_t message;
    try {
        if (group) {
            auto saved = store.save_group_message(sender, client_id, *group, text);
            if (!saved) return {make_error_response(request, "not_group_member", "Group does not exist or you are not a member")};
            message = saved->id;
            recipients = std::move(saved->recipients);
        } else {
            if (store.existing_user_ids(recipients) != recipients) {
                return {make_error_response(request, "invalid_recipient", "A recipient does not exist")};
            }
            message = store.save_direct_message(sender, client_id, text, recipients);
        }
    } catch (const std::invalid_argument&) {
        return {make_error_response(request, "message_conflict", "Client message ID belongs to a different message")};
    }
    nlohmann::json result{{"message_id", std::to_string(message)}};
    nlohmann::json data{{"message_id", std::to_string(message)}, {"sender_id", std::to_string(sender)},
                        {"client_message_id", client_id}, {"text", text}};
    if (group) {
        result["group_id"] = std::to_string(*group);
        data["group_id"] = std::to_string(*group);
    }
    const auto notification = make_notification("message", std::move(data));
    return {make_response(request, std::move(result)), std::nullopt,
            UserNotification{std::move(recipients), notification.dump()}};
}

RequestResult RequestHandler::create_group(const Request& request, std::uint64_t owner) const {
    const auto& name = require_string(request, "name", 128);
    auto members = parse_ids(request, "members", true);
    members.push_back(owner);
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() > 64) return {make_error_response(request, "invalid_request", "A group accepts at most 64 members including its owner")};
    ChatStore store(database());
    if (store.existing_user_ids(members) != members) {
        return {make_error_response(request, "invalid_recipient", "A member does not exist")};
    }
    const auto group = store.create_group(owner, name, members);
    const auto notice = make_notification("group_created", {{"group_id", std::to_string(group)},
        {"owner_id", std::to_string(owner)}, {"name", name}, {"members", id_strings(members)}});
    members.erase(std::find(members.begin(), members.end(), owner));
    return {make_response(request, {{"group_id", std::to_string(group)}}), std::nullopt,
            UserNotification{std::move(members), notice.dump()}};
}

RequestResult RequestHandler::group_members(const Request& request, std::uint64_t user) const {
    const auto group = require_id(request, "group_id");
    ChatStore store(database());
    const auto members = store.group_members(group);
    if (!std::binary_search(members.begin(), members.end(), user)) {
        return {make_error_response(request, "not_group_member", "Group does not exist or you are not a member")};
    }
    return {make_response(request, {{"group_id", std::to_string(group)}, {"members", id_strings(members)}})};
}

RequestResult RequestHandler::acknowledge_message(const Request& request, std::uint64_t recipient) const {
    const auto value = request.data.find("message_id");
    const auto id = value == request.data.end() ? std::nullopt : parse_id(*value);
    if (!id) {
        return {make_error_response(request, "invalid_request", "message_id must be a positive message ID string")};
    }
    ChatStore store(database());
    if (!store.acknowledge_message(recipient, *id)) {
        return {make_error_response(request, "invalid_message", "Message delivery does not exist")};
    }
    return {make_response(request, {{"message_id", std::to_string(*id)}}), std::nullopt,
            std::nullopt, *id};
}

std::vector<MessageDelivery> RequestHandler::pending_messages(std::uint64_t recipient) const {
    try {
        ChatStore store(database());
        std::vector<MessageDelivery> deliveries;
        std::size_t bytes = 0;
        for (const auto& message : store.pending_messages(recipient, kReplayBatchCount)) {
            nlohmann::json data{{"message_id", std::to_string(message.id)},
                {"sender_id", std::to_string(message.sender_id)},
                {"client_message_id", message.client_message_id}, {"text", message.text}};
            if (message.group_id) data["group_id"] = std::to_string(*message.group_id);
            auto payload = make_notification("message", std::move(data)).dump();
            if (payload.size() > kMaxFrameSize) {
                throw std::length_error("Stored message exceeds the 64 KiB frame limit");
            }
            if (bytes + payload.size() + 4 > kReplayBatchBytes) break;
            bytes += payload.size() + 4;
            deliveries.push_back({message.id, std::move(payload)});
        }
        return deliveries;
    } catch (const DatabaseError&) {
        worker_database().reset();
        throw;
    }
}

} // namespace chatroom
