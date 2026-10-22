#include "console_client.h"
#include "protocol.h"

#include <cerrno>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>

namespace chatroom {
namespace {
const char* kHelp = "Commands:\n"
    "  /register <username> <password>  Create an account\n"
    "  /login <username> <password>     Log in and authenticate TCP\n"
    "  /send <id[,id...]> <text>        Send a message to user IDs\n"
    "  /create_group <name> [id,...]    Create a group, including yourself\n"
    "  /members <group-id>             List the group's members\n"
    "  /send_group <group-id> <text>    Send a group message\n"
    "  /upload <id[,id...]> <path>      Upload a file for selected users\n"
    "  /download <file-id> <path>       Save a file to a new local path\n"
    "  /ping                           Check the TCP connection\n"
    "  /help                           Show these commands\n"
    "  /quit                           Disconnect\n"
    "Quote credentials, group names, or paths containing spaces. JSON requests are also accepted.\n";

bool valid_id(std::string_view text) {
    std::uint64_t id = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           id != 0 && text.front() != '0';
}

nlohmann::json parse_ids(std::string_view text) {
    auto ids = nlohmann::json::array();
    if (text.empty()) return ids;
    while (true) {
        const auto comma = text.find(',');
        const auto id = text.substr(0, comma);
        if (!valid_id(id)) throw std::invalid_argument("Use positive user IDs separated by commas");
        ids.push_back(std::string(id));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (ids.size() > 64) throw std::invalid_argument("At most 64 user IDs are allowed");
    return ids;
}

std::string random_prefix() {
    std::random_device random;
    std::ostringstream prefix;
    prefix << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) prefix << std::setw(8) << random();
    return prefix.str();
}

std::string field(const nlohmann::json& object, const char* name) {
    const auto value = object.find(name);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
}

// Keep UTF-8 readable while escaping terminal controls, newlines, and backslashes.
std::string visible(const std::string& text) {
    const auto encoded = nlohmann::json(text).dump();
    return encoded.substr(1, encoded.size() - 2);
}

std::string format_message(const nlohmann::json& message) {
    const auto type = field(message, "type");
    const auto action = field(message, "action");
    if (type == "response" && message.value("ok", nlohmann::json{}) == false) {
        const auto error = message.find("error");
        if (error != message.end() && error->is_object()) {
            return "Error [" + visible(action) + "]: " + visible(field(*error, "code")) +
                   ": " + visible(field(*error, "message"));
        }
    }
    const auto data = message.find("data");
    if (data == message.end() || !data->is_object()) return "Server: " + message.dump();
    if (type == "notification" && action == "message") {
        const auto group = field(*data, "group_id");
        const auto prefix = group.empty() ? "[message " : "[group " + visible(group) + ", message ";
        return prefix + visible(field(*data, "message_id")) + "] user " +
               visible(field(*data, "sender_id")) + ": " + visible(field(*data, "text"));
    }
    if (type == "notification" && action == "group_created") {
        return "Added to group " + visible(field(*data, "group_id")) + ": " +
               visible(field(*data, "name")) + " (owner " + visible(field(*data, "owner_id")) + ").";
    }
    if (type == "notification" && action == "file_available") {
        return "[file " + visible(field(*data, "file_id")) + ", message " + visible(field(*data, "message_id")) +
            "] user " + visible(field(*data, "sender_id")) + ": " + visible(field(*data, "name")) +
            " (" + data->value("size_bytes", nlohmann::json{}).dump() + " bytes). Use /download " +
            visible(field(*data, "file_id")) + " <path>.";
    }
    if (type == "response" && message.value("ok", nlohmann::json{}) == true) {
        if (action == "ack_message") return {};
        if (action == "authenticate") return "Authenticated as user " + visible(field(*data, "user_id")) + ".";
        if (action == "send_message") return "Sent message " + visible(field(*data, "message_id")) + ".";
        if (action == "send_group_message") return "Sent message " + visible(field(*data, "message_id")) +
            " to group " + visible(field(*data, "group_id")) + ".";
        if (action == "create_group") return "Created group " + visible(field(*data, "group_id")) + ".";
        if (action == "get_group_members") {
            const auto members = data->find("members");
            if (members != data->end() && members->is_array()) {
                std::string text = "Group " + visible(field(*data, "group_id")) + " members: ";
                for (std::size_t i = 0; i < members->size(); ++i) {
                    if (!(*members)[i].is_string()) return "Server: " + message.dump();
                    if (i != 0) text += ", ";
                    text += visible((*members)[i].get<std::string>());
                }
                return text + ".";
            }
        }
        if (action == "ping") return "Pong.";
    }
    return "Server: " + message.dump();
}

std::optional<std::string> notification_id(const nlohmann::json& message) {
    if (!message.is_object() || message.value("type", nlohmann::json{}) != "notification") return std::nullopt;
    const auto action = message.value("action", nlohmann::json{});
    if (action != "message" && action != "file_available") return std::nullopt;
    const auto data = message.find("data");
    if (data == message.end() || !data->is_object()) return std::nullopt;
    for (const auto* name : {"sender_id"}) {
        const auto value = data->find(name);
        if (value == data->end() || !value->is_string()) return std::nullopt;
    }
    if (action == "message") {
        for (const auto* name : {"client_message_id", "text"}) {
            const auto value = data->find(name);
            if (value == data->end() || !value->is_string()) return std::nullopt;
        }
    } else {
        for (const auto* name : {"file_id", "name", "content_type"}) {
            const auto value = data->find(name);
            if (value == data->end() || !value->is_string()) return std::nullopt;
        }
        if (!valid_id(field(*data, "file_id")) || !data->contains("size_bytes") ||
            !(*data)["size_bytes"].is_number_unsigned()) return std::nullopt;
    }
    const auto value = data->find("message_id");
    if (value == data->end() || !value->is_string()) return std::nullopt;
    const auto& text = value->get_ref<const std::string&>();
    if (valid_id(text)) return text;
    return std::nullopt;
}
} // namespace

ConsoleClient::ConsoleClient(TcpClient& client, AuthClient& auth, FileClient& files, bool json_output)
    : client_(client), auth_(auth), files_(files), json_output_(json_output), message_prefix_(random_prefix()),
      receiver_done_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (receiver_done_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
}

void ConsoleClient::print(std::string_view text) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    std::cout << text << std::flush;
}

bool ConsoleClient::send_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line == "/quit") {
        return false;
    }
    if (line.empty()) {
        return true;
    }
    if (line.front() == '/') {
        try { return handle_command(line); }
        catch (const std::invalid_argument& error) { print(std::string("Error: ") + error.what() + "\n> "); }
        return true;
    }
    if (!nlohmann::json::accept(line.begin(), line.end())) {
        print("Invalid input. Use /help for commands, or enter a JSON request.\n> ");
    } else {
        std::lock_guard<std::mutex> lock(send_mutex_);
        client_.send(line);
        print("> ");
    }
    return true;
}

void ConsoleClient::send_request(const std::string& action, nlohmann::json data) {
    const auto number = std::to_string(next_request_id_++);
    const auto token = action == "authenticate" ? data.at("token").get<std::string>() : std::string{};
    if (action == "send_message" || action == "send_group_message") {
        data["client_message_id"] = message_prefix_ + "-" + number;
    }
    const auto payload = nlohmann::json{{"type", "request"}, {"request_id", "cli-" + number},
                                       {"action", action}, {"data", std::move(data)}}.dump();
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!token.empty()) pending_tokens_["cli-" + number] = token;
    client_.send(payload);
}

bool ConsoleClient::handle_command(std::string_view line) {
    std::istringstream input{std::string(line)};
    std::string command, extra;
    input >> command;
    if (command == "/help" || command == "/ping" || command == "/quit") {
        if (input >> extra) throw std::invalid_argument("This command takes no arguments");
        if (command == "/quit") return false;
        if (command == "/help") print(kHelp);
        else send_request("ping");
    } else if (command == "/register" || command == "/login") {
        std::string username, password;
        if (!(input >> std::quoted(username) >> std::quoted(password)) || (input >> extra)) {
            throw std::invalid_argument("Usage: " + command + " <username> <password>");
        }
        std::string token;
        try {
            if (command == "/register") {
                const auto id = auth_.register_user(username, password);
                print("Registered user " + visible(id) + ". Use /login to sign in.\n> ");
                return true;
            }
            token = auth_.login(username, password);
        } catch (const std::exception& error) {
            print("Error: " + visible(error.what()) + "\n> ");
            return true;
        }
        send_request("authenticate", {{"token", token}});
    } else if (command == "/upload" || command == "/download") {
        std::string target, path;
        if (!(input >> target >> std::quoted(path)) || path.empty() || (input >> extra)) {
            throw std::invalid_argument("Usage: " + command + " " +
                (command == "/upload" ? "<id[,id...]> <path>" : "<file-id> <path>"));
        }
        auto recipients = command == "/upload" ? parse_ids(target) : nlohmann::json::array();
        if (command == "/download" && !valid_id(target)) throw std::invalid_argument("File ID must be a positive ID string");
        std::string token;
        { std::lock_guard<std::mutex> lock(send_mutex_); token = token_; }
        try {
            if (command == "/upload") {
                const auto saved = files_.upload(token, path, std::move(recipients));
                print("Uploaded file " + visible(saved.at("file_id").get<std::string>()) + ".\n");
            } else {
                files_.download(token, target, path);
                print("Downloaded file " + target + " to " + visible(path) + ".\n");
            }
        } catch (const std::exception& error) {
            print("Error: " + visible(error.what()) + "\n");
        }
    } else if (command == "/create_group") {
        std::string name, members;
        if (!(input >> std::quoted(name))) throw std::invalid_argument("Usage: /create_group <name> [id,...]");
        input >> members;
        if (input >> extra) throw std::invalid_argument("Quote group names containing spaces");
        if (name.empty() || name.size() > 128) throw std::invalid_argument("Group name must contain 1 to 128 bytes");
        send_request("create_group", {{"name", name}, {"members", parse_ids(members)}});
    } else if (command == "/members") {
        std::string group;
        if (!(input >> group) || !valid_id(group) || (input >> extra)) {
            throw std::invalid_argument("Usage: /members <group-id>");
        }
        send_request("get_group_members", {{"group_id", group}});
    } else if (command == "/send" || command == "/send_group") {
        std::string target, text;
        input >> target;
        std::getline(input >> std::ws, text);
        if (target.empty() || text.empty()) {
            throw std::invalid_argument("Usage: " + command + " " +
                (command == "/send" ? "<id[,id...]> <text>" : "<group-id> <text>"));
        }
        if (text.size() > 4096) throw std::invalid_argument("Message text must contain 1 to 4096 bytes");
        if (command == "/send_group") {
            if (!valid_id(target)) throw std::invalid_argument("Group ID must be a positive ID string");
            send_request("send_group_message", {{"group_id", target}, {"text", text}});
        } else {
            send_request("send_message", {{"recipients", parse_ids(target)}, {"text", text}});
        }
    } else {
        throw std::invalid_argument("Unknown command. Use /help to list commands");
    }
    print("> ");
    return true;
}

ConsoleClient::InputResult ConsoleClient::read_input() {
    // The receiver wakes poll on disconnect, even while the user has not entered a line.
    pollfd events[] = {{STDIN_FILENO, POLLIN, 0}, {receiver_done_.get(), POLLIN, 0}};
    std::string input;
    bool discarding_line = false;
    char buffer[4096];
    while (true) {
        if (::poll(events, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (events[1].revents & POLLIN) {
            return InputResult::disconnected;
        }
        if (events[0].revents & (POLLERR | POLLNVAL)) {
            throw std::runtime_error("Standard input is unavailable");
        }
        if (!(events[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }

        const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "read(stdin)");
        }
        if (count == 0) {
            if (discarding_line) return InputResult::eof;
            return send_line(input) ? InputResult::eof : InputResult::quit;
        }
        for (const char character : std::string_view(buffer, static_cast<std::size_t>(count))) {
            if (character == '\n') {
                if (!discarding_line && !send_line(input)) return InputResult::quit;
                input.clear();
                discarding_line = false;
            } else if (!discarding_line) {
                // Allow one trailing CR beyond the payload limit for CRLF input.
                if (input.size() < kMaxFrameSize ||
                    (input.size() == kMaxFrameSize && character == '\r')) {
                    input += character;
                } else {
                    input.clear();
                    discarding_line = true;
                    print("Message exceeds the 64 KiB limit.\n> ");
                }
            }
        }
    }
}

void ConsoleClient::display_message(std::string_view payload) {
    nlohmann::json message;
    try { message = parse_json(payload); }
    catch (const ProtocolError&) {
        print(json_output_ ? "\n< " + std::string(payload) + "\n> " : "\nInvalid server JSON.\n> ");
        return;
    }
    if (message.is_object() && field(message, "type") == "response" && field(message, "action") == "authenticate") {
        std::lock_guard<std::mutex> lock(send_mutex_);
        const auto pending = pending_tokens_.find(field(message, "request_id"));
        if (pending != pending_tokens_.end()) {
            if (message.value("ok", nlohmann::json{}) == true) token_ = std::move(pending->second);
            pending_tokens_.erase(pending);
        }
    }
    const auto id = notification_id(message);
    if (!id) {
        const auto text = json_output_ ? "< " + std::string(payload) : format_message(message);
        if (!text.empty()) print("\n" + text + "\n> ");
        return;
    }
    if (received_messages_.insert(*id).second) {
        recent_messages_.push_back(*id);
        // A bounded, in-memory history; older duplicates can be displayed again.
        if (recent_messages_.size() > 1024) {
            received_messages_.erase(recent_messages_.front());
            recent_messages_.pop_front();
        }
        print("\n" + (json_output_ ? "< " + std::string(payload) : format_message(message)) + "\n> ");
    }
    const auto ack = nlohmann::json{{"type", "request"}, {"request_id", "ack-" + *id},
        {"action", "ack_message"}, {"data", {{"message_id", *id}}}}.dump();
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!sending_finished_) client_.send(ack);
}

void ConsoleClient::receive_messages() {
    try {
        while (auto payload = client_.receive()) {
            display_message(*payload);
        }
    } catch (...) {
        receive_error_ = std::current_exception();
    }

    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = ::write(receiver_done_.get(), &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

void ConsoleClient::run() {
    print("Use /help for commands. JSON requests are also accepted.\n> ");
    std::thread receiver([this] { receive_messages(); });
    InputResult result;
    try {
        result = read_input();
        if (result == InputResult::eof) {
            std::lock_guard<std::mutex> lock(send_mutex_);
            sending_finished_ = true;
            client_.finish_sending();
        } else {
            client_.disconnect();
        }
    } catch (...) {
        client_.disconnect();
        receiver.join();
        throw;
    }
    receiver.join();
    // Local cancellation can interrupt a partially received frame.
    if (result != InputResult::quit && receive_error_) {
        std::rethrow_exception(receive_error_);
    }
    print("\nDisconnected.\n");
}

} // namespace chatroom
